"""Turn native boundary evidence into the common coarse-projection input."""

from __future__ import annotations

from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
import json
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.commands import NativeStreamCall, StreamCommand, StreamCommands
from markov_collection.adapters.runtimes.ascend.host_alignment import ThreadService
from markov_collection.adapters.runtimes.ascend.trace import AscendTrace, Interval, ThreadIntervals
from markov_collection.instrumentation.observation import ObservationReferences


class AscendObservation:
    def __init__(self, directory: Path, rank: int, raw: dict, trace: Path) -> None:
        self.raw = raw
        self.rank = rank
        if raw["environment"]["parallel"]["rank"] != rank:
            raise ValueError("worker recording differs from the requested rank")
        activity = raw["host_observation"].get("activity")
        if activity is not None and activity != f"rank-{rank}-thread-activity.jsonl":
            raise ValueError("CPU activity file differs from the observed rank")
        self.activity_path = None if activity is None else directory / activity
        profile, = (directory / "native" / f"rank-{rank}").glob("*_ascend_pt")
        self.trace = AscendTrace(profile, directory / "runtime", raw, trace)
        self.streams = StreamCommands(self.trace)
        self.control_threads = {interval.thread for scope, interval in self.trace.scopes.items() if "service" not in raw["scopes"][scope]}
        self.resources = []
        self.resource_ids = {}
        self.stream_names = {}
        self.spans = []
        self.dependencies = []
        self.host = {}
        self.callers = {}
        self.device = {}
        self.services = {}
        self.payload = []
        self.collectives = {}
        self.host_copies = []
        self.signal_inputs = {}
        self.work_completions = {}
        self.scopes = []
        for identity, scope in enumerate(raw["scopes"]):
            if scope["id"] != identity or not scope["complete"]:
                raise ValueError(f"native scope {identity} is not a completed, ordered call")
            info = {name: scope[name] for name in (
                "parent", "role", "site", "implementation", "parameters", "constants", "arguments", "work", "bindings",
            )}
            info["bindings"] = dict(info["bindings"])

            info["guards"] = scope.get("guards", [])
            for name in ("effects", "publication"):
                if name in scope:
                    info["bindings"][name] = scope[name]

            # Raw and interpreted scope IDs have separate consumers; record their owner at production.
            info["bindings"]["source_scopes"] = [identity]
            if "communication" in scope:
                info["communication"] = scope["communication"]
            info["ports"] = []
            info["kv_accesses"] = []
            info["slot_accesses"] = []
            self.scopes.append(info)
        if set(self.trace.scopes) != set(range(1, len(self.scopes))):
            raise ValueError("native scope metadata and recorded CPU markers differ")

    def name_streams(self, points: dict[int, str], waits: dict[int, dict[str, str]]) -> None:
        if not points and not waits:
            return
        trace = self.trace
        declarations = [(trace.device_points[point].stream, role) for point, role in points.items()]
        observed = set()
        for native, recorded in trace.synchronizations:
            if native.record["api"] not in trace.stream_waits:
                continue
            owner = trace.owner(trace.origin(trace.native_api(native)))
            if owner not in waits:
                continue
            if recorded is None:
                raise ValueError("named stream wait has no native event generation")
            task, = trace.tasks[trace.native_api(recorded).record["args"]["connection_id"]]
            if (task.record["args"]["Task Type"] != "EVENT_RECORD" or recorded.record["stream_status"] != 0
                    or recorded.record["stream"] != task.stream or native.record["stream_status"] != 0):
                raise ValueError("named stream wait changes its recorded producer stream")
            declarations.extend(((task.stream, waits[owner]["source"]), (native.record["stream"], waits[owner]["target"])))
            observed.add(owner)
        if observed != waits.keys():
            raise ValueError("logical stream wait has no observed native wait")

        # The framework's canonical names must identify the same lanes throughout this worker's observation.
        named = {}
        for stream, role in declarations:
            if self.stream_names.setdefault(stream, role) != role or named.setdefault(role, stream) != stream:
                raise ValueError("native logical streams have conflicting resource bindings")

    def resource(self, kind: str, identity: int | str, role: str, *, serial: bool = True) -> int:
        key = kind, identity
        if key not in self.resource_ids:
            if kind == "thread":
                role = "control" if identity in self.control_threads else "submission"
            elif kind == "stream":
                role = self.stream_names.get(identity, role)
            self.resource_ids[key] = len(self.resources)
            self.resources.append({
                "kind": kind, "rank": self.rank, "role": role, "identity": f"{kind}/{identity}",
                "serial": serial, "observed": True, "payload": [],
            })
        return self.resource_ids[key]

    def span(self, scope: int, resource: int, begin: int, end: int, kind: str) -> int:
        if begin > end:
            raise ValueError(f"reversed native {kind} interval in scope {scope}")
        identity = len(self.spans)
        self.spans.append({"scope": scope, "resource": resource, "begin": begin, "end": end, "kind": kind})
        return identity

    @staticmethod
    def at(span: int, order: int) -> dict[str, int]:
        return {"span": span, "order": order}

    def endpoint(self, span: int, side: str) -> dict[str, int]:
        return self.at(span, self.spans[span][side])

    def edge(self, source: dict, target: dict, kind: str) -> None:
        if source != target:
            self.dependencies.append({"source": source, "target": target, "kind": kind})

    def host_anchor(self, interval: Interval, side: str = "begin") -> dict[str, int]:
        position = getattr(interval, side)
        boundary = self.host_index.enclosing(Interval(position, position, interval.thread, {}))
        if boundary is None:
            raise ValueError(f"required host endpoint has no observed scope: thread {interval.thread} at {position}")
        return self.at(boundary.record["span"], position)

    def span_index(self, kind: str) -> ThreadIntervals:
        intervals = []
        for identity, span in enumerate(self.spans):
            resource = self.resources[span["resource"]]
            if resource["kind"] == kind:
                coordinate = int(resource["identity"].split("/")[1])
                intervals.append(Interval(span["begin"], span["end"], coordinate, {"span": identity}))
        return ThreadIntervals(intervals)

    def host_spans(self) -> None:
        queries = {query["scope"]: query for query in self.raw["state_queries"]}

        # Coordinates express ordering, not service time; observer calls must not fragment business CPU work.
        for scope, interval in self.trace.scopes.items():
            if self.raw["scopes"][scope]["thread"] != interval.thread:
                raise ValueError(f"scope {scope} differs from its recorded native thread")
            resource = self.resource("thread", interval.thread, "native")
            raw = self.raw["scopes"][scope]
            kind = "wait" if "service" in raw else "boundary" if raw.get("envelope", False) else "host"
            exchange = raw.get("communication")
            if exchange is not None and "device_entry" not in raw:

                # Charge all-peer service after rendezvous; asymmetric calls retain their own completion rules.
                kind = "collective" if exchange["required"] == exchange["members"] else "communication"
                if interval.begin < interval.end:
                    self.resources[resource]["payload"].append([interval.begin, interval.end])
            self.host[scope] = self.span(scope, resource, interval.begin, interval.end, kind)
            if scope in queries:
                read = self.trace.native_interval({**queries[scope], "thread": interval.thread})
                if not interval.contains(read):
                    raise ValueError(f"query {scope} escapes its observed caller interval")

                # The actual read returns before observer bookkeeping; retain its caller envelope without charging it.
                self.callers[scope] = self.host[scope]
                self.spans[self.host[scope]]["kind"] = "boundary"
                self.host[scope] = self.span(scope, resource, read.begin, read.end, "host")
        self.host_index = self.span_index("thread")
        for dequeue in sorted(self.trace.dequeue.values(), key=lambda value: (value.thread, value.begin)):
            enqueue = self.trace.enqueue[dequeue.record["args"]["correlation_id"]]
            if self.trace.excluded_ranges.enclosing(enqueue) is not None:
                continue
            scope = self.trace.owner(enqueue)
            if scope is None or scope in self.trace.closed_scopes:
                continue

            # Handle maintenance remains lifecycle evidence, not an extra submission segment between business tasks.
            apis = self.trace.api_ranges.inside(dequeue)
            if not any(self.trace.tasks.get(api.record["args"]["connection_id"])
                       or api.record["args"]["connection_id"] in self.streams.stream_calls
                       or api.record["args"]["id"] in self.trace.host_waits | {"aclrtMemcpy", "aclrtMemcpyAsync"} for api in apis):
                continue
            resource = self.resource("thread", dequeue.thread, "native")

            # Each take has its own producer, even when adjacent commands belong to the same scope.
            span = self.span(scope, resource, dequeue.begin, dequeue.end, "host")
            self.edge(self.host_anchor(enqueue), self.endpoint(span, "begin"), "submission")

        for native in self.trace.runtime:
            if native.record["api"] != "aclrtMemcpy" or native.record["bytes"] == 0:
                continue

            # The observation window covers every worker thread, not only its opening caller.
            if native.begin < self.trace.window.begin or self.trace.window.end < native.end:
                continue
            origin = self.trace.origin(self.trace.native_api(native))
            owner = self.trace.owner(origin)
            if owner is not None and owner not in self.trace.closed_scopes and self.trace.excluded_ranges.enclosing(origin) is None:
                self.host_transfer(owner, native, native.record["direction"])
        self.host_index = self.span_index("thread")

    def host_transfer(self, scope: int, interval: Interval, direction: int) -> None:
        resource = self.resource("thread", interval.thread, "native")
        span = self.span(scope, resource, interval.begin, interval.end, "transfer")
        self.resources[resource]["payload"].append([interval.begin, interval.end])
        self.host_copies.append((scope, span, interval, direction))

    def service_spans(self) -> None:
        for scope, raw in enumerate(self.raw["scopes"]):
            if "service" not in raw:
                continue
            interval = self.trace.scopes[scope]
            service = raw["service"]
            resource = self.resource("service", interval.thread, service["role"], serial=False)

            # A synchronous service joins its private workers; their incidental dispatch is not an external interface.
            span = self.span(scope, resource, interval.begin, interval.end, service["kind"])
            if interval.begin < interval.end:
                self.resources[resource]["payload"].append([interval.begin, interval.end])
            self.services[scope] = span
            self.edge(self.endpoint(self.host[scope], "begin"), self.endpoint(span, "begin"), "submission")
            self.edge(self.endpoint(span, "end"), self.endpoint(self.host[scope], "end"), "signal")
            if "submitted_by" in raw:
                producer = self.host[raw["submitted_by"]]
                self.edge(self.endpoint(producer, "end"), self.endpoint(span, "begin"), "submission")

    def transfer_times(self) -> list:
        samples = [
            [self.spans[span]["resource"], interval.begin, interval.end,
             interval.record["end_raw_ns"] - interval.record["begin_raw_ns"], interval.record["bytes"], direction]
            for _, span, interval, direction in self.host_copies
        ]
        for scope, identity in self.services.items():
            raw = self.raw["scopes"][scope]
            service = raw["service"]
            payload = service.get("payload")
            if not payload or not payload["bytes"] or not service["executions"]:
                continue
            span, stamps = self.spans[identity], raw["host_interval"]

            # Queue attribution already checked the private submissions. Its synchronous return joins the workers.
            samples.append([span["resource"], span["begin"], span["end"], stamps["end_raw_ns"] - stamps["begin_raw_ns"],
                            payload["bytes"], payload["direction"]])
        return samples

    def device_spans(self) -> None:
        points = self.trace.device_points
        if set(points) != set(range(len(self.raw["points"]))):
            raise ValueError("device point metadata and native markers differ")
        for scope, raw in enumerate(self.raw["scopes"]):
            if "device_entry" not in raw:
                continue
            first, last = raw["device_entry"], raw["device_return"]
            for identity, side in ((first, "entry"), (last, "return")):
                if self.raw["points"][identity] != {"scope": scope, "side": side}:
                    raise ValueError(f"scope {scope} refers to another call's device point")
            begin, end = points[first], points[last]
            if begin.stream != end.stream:
                raise ValueError(f"scope {scope} changes its boundary stream; explicit stream ports are required")
            resource = self.resource("stream", begin.stream, "execution")
            kind = "boundary" if raw.get("envelope", False) else "communication" if "communication" in raw else "device"
            span = self.span(scope, resource, self.streams.task(begin.task).end, self.streams.task(end.task).end, kind)
            self.device[scope] = span

        # A native event may be recorded on another stream than its enclosing action's device interval.
        for identity, observed in enumerate(self.raw["points"]):
            if observed["side"] != "signal":
                continue
            point = points[identity]
            resource = self.resource("stream", point.stream, "execution")
            position = self.streams.task(point.task).end
            self.span(observed["scope"], resource, position, position, "boundary")

        self.device_index = self.span_index("stream")
        self.auxiliary_stream_spans()

        self.device_index = self.span_index("stream")
        for source in self.streams.native_calls:
            if source.completion is None:
                continue

            # A successful driver transfer is caller work, not a queued device command.
            self.host_transfer(source.scope, source.completion, source.native.record["direction"])
        for scope, transfer, _, _ in self.host_copies:

            # An observation envelope cannot promote synchronous caller work into a device-stream fence.
            while scope != 0 and (scope not in self.device or self.raw["scopes"][scope].get("envelope", False)):
                scope = self.scopes[scope]["parent"]
            if scope in self.device:
                span = self.device[scope]
                boundary = self.spans[span]

                # Preserve the actual return fence as a join; never retarget CPU completion to an earlier device task.
                self.span(scope, boundary["resource"], boundary["end"], boundary["end"], "boundary")
                self.edge(self.endpoint(transfer, "end"), self.endpoint(span, "end"), "signal")
        self.resolve_work_completions()
        for task in self.streams.payload:
            source = task.source
            kind = source.record["args"]["Task Type"]
            if kind in {"EVENT_RECORD", "EVENT_WAIT", "PROFILING_ENABLE", "PROFILING_DISABLE"}:
                continue
            if source.service is not None or self.trace.auxiliary(source) or self.trace.outside_window(source):
                continue
            stream = task.stream
            resource = self.resource("stream", stream, "execution")
            if self.device_index.enclosing(Interval(task.begin, task.end, stream, {})) is None:
                raise ValueError(f"native payload has no semantic stream coverage: stream {stream} at {task.begin}")
            self.payload.append(task)
            self.resources[resource]["payload"].append([task.begin, task.end])

    def collective_spans(self) -> None:
        groups = defaultdict(dict)
        for task in self.payload:
            api = self.trace.apis.get(task.source.record["args"]["connection_id"])
            if api is None:
                continue
            queue = self.trace.dequeue_ranges.enclosing(api)
            if queue is None or not queue.record["name"].startswith("Dequeue@Hccl"):
                continue
            scope = self.trace.owner(self.trace.origin(api))
            if scope is None:
                continue
            raw = self.raw["scopes"][scope]
            exchange = raw.get("communication")
            if not exchange or (exchange["required"] != exchange["members"] and "peer_rows" not in raw):
                continue
            if raw.get("asynchronous", False) and exchange["operation"] != "transfer":
                continue
            groups[scope].setdefault(queue.record["args"]["correlation_id"], []).append(task)

        spans = defaultdict(list)
        for span in self.spans:
            if span["kind"] == "communication" and self.resources[span["resource"]]["kind"] == "stream":
                spans[span["scope"]].append(span)
        for scope, calls in groups.items():
            if len(calls) != 1:
                continue
            (correlation, tasks), = calls.items()
            stream = tasks[0].stream
            if any(task.stream != stream for task in tasks):
                continue
            begin, end = tasks[0].begin, tasks[-1].end
            limit = self.device_index.enclosing(Interval(begin, end, stream, {}))
            if limit is None:
                continue
            enclosing = self.spans[limit.record["span"]]
            if enclosing["scope"] != scope or enclosing["kind"] != "communication":
                continue

            # Keep caller-stream work such as the gather transpose outside the peer-gated service.
            for span in spans[scope]:
                span["kind"] = "device"
            resource = enclosing["resource"]
            if (enclosing["begin"], enclosing["end"]) == (begin, end):
                enclosing["kind"] = "collective"
            else:
                self.span(scope, resource, begin, end, "collective")
            self.collectives[scope] = (resource, begin, end, self.trace.dequeue[correlation])
        self.device_index = self.span_index("stream")

    def auxiliary_stream_spans(self) -> None:
        pending = None
        communications = []
        for task in self.streams.commands:
            source = task.source
            if isinstance(source, NativeStreamCall):
                resource = self.resource("stream", task.stream, "execution")
                self.span(source.scope, resource, task.begin, task.end, "wait")
                pending = None
                continue
            else:
                if source.service is not None or self.trace.auxiliary(source) or self.trace.outside_window(source):
                    continue
                api = self.trace.apis.get(source.record["args"]["connection_id"])
                owner = None if api is None else self.trace.owner(self.trace.origin(api))
            stream = task.stream
            if self.device_index.enclosing(Interval(task.begin, task.end, stream, {})) is not None:
                pending = None
                continue
            if owner is None:
                pending = None
                continue
            resource = self.resource("stream", stream, "execution")
            key = owner, stream
            if pending is not None and pending[0] == key:
                self.spans[pending[1]]["end"] = task.end
            else:
                communication = "communication" in self.raw["scopes"][owner]
                span = self.span(owner, resource, task.begin, task.end, "communication" if communication else "device")
                pending = key, span
                if communication:
                    communications.append((owner, span))

        # Other streams belong to the submitting scope; their actual event/wait records supply completion dependencies.
        for scope, span in communications:

            # Native tensor collectives publish usable output on their caller stream, even with internal communication streams.
            caller = self.device[scope]
            self.edge(self.endpoint(caller, "begin"), self.endpoint(span, "begin"), "data")
            if not self.raw["scopes"][scope].get("asynchronous", False):
                self.edge(self.endpoint(span, "end"), self.endpoint(caller, "end"), "signal")

    def resolve_work_completions(self) -> None:
        producers = {scope for scope, raw in enumerate(self.raw["scopes"]) if raw.get("asynchronous", False)}
        if not producers:
            return
        consumers = {
            scope: port["producer"]
            for scope, raw in enumerate(self.raw["scopes"])
            for port in raw["ports"] if port["use"] == "signal" and port["direction"] == "input" and "producer" in port
        }
        for native, recorded in self.trace.synchronizations:
            if recorded is None:
                continue
            owner = self.trace.owner(self.trace.origin(self.trace.native_api(native)))
            if owner not in consumers:
                continue
            producer = consumers[owner]
            api = self.trace.native_api(recorded)
            if self.trace.owner(self.trace.origin(api)) != producer:
                continue
            task, = self.trace.tasks[api.record["args"]["connection_id"]]
            task = self.streams.task(task)
            previous = self.work_completions.setdefault(producer, task)
            if previous is not task:
                raise ValueError(f"native Work {producer} waits on more than one completion event")

        for scope in producers:
            if scope not in self.work_completions:
                raise ValueError(f"native Work {scope} has no observed completion wait")

    def call_ports(self) -> None:
        observed = []
        for scope, raw in enumerate(self.raw["scopes"]):
            if scope == 0:
                continue
            host = self.host[scope]
            ports = self.scopes[scope]["ports"]
            for name, direction, side in (("call.enter", "input", "begin"), ("call.return", "output", "end")):
                ports.append({
                    "name": name, "direction": direction, "use": "control", "relation": "control",
                    "at": self.endpoint(host, side),
                })
            if scope in self.callers:
                ports.append({
                    "name": "caller.return", "direction": "output", "use": "control", "relation": "control",
                    "at": self.endpoint(self.callers[scope], "end"),
                })
            if scope in self.services:
                ports.append({
                    "name": "service.complete", "direction": "output", "use": "control", "relation": "control",
                    "at": self.endpoint(self.services[scope], "end"),
                })
            if scope in self.device:
                for name, direction, side in (("device.enter", "input", "begin"), ("device.complete", "output", "end")):
                    ports.append({
                        "name": name, "direction": direction, "use": "control", "relation": "control",
                        "at": self.endpoint(self.device[scope], side),
                    })
            if "communication" in raw:
                span = self.device[scope] if "device_entry" in raw else host
                completion = self.task_anchor(self.work_completions[scope]) if raw.get("asynchronous", False) else self.endpoint(span, "end")
                ports.append({
                    "name": "communication.complete", "direction": "output", "use": "control", "relation": "communication",
                    "at": completion,
                    "rendezvous": self.endpoint(span, "begin"),
                })
            for port in raw["ports"]:
                if port["use"] == "signal":
                    if "boundary" in port:
                        boundary = port["boundary"]
                        span = {"call": self.host, "device": self.device, "service": self.services}[boundary["interval"]][boundary["scope"]]
                        ports.append({
                            "name": port["name"], "direction": port["direction"], "use": "signal", "relation": "signal",
                            "signal": f"scope/{boundary['scope']}/{boundary['interval']}/{boundary['side']}",
                            "at": self.endpoint(span, boundary["side"]),
                        })
                        continue
                    task = self.work_completions[port["producer"]] if "producer" in port else self.streams.task(self.trace.device_points[port["point"]].task)
                    connection = task.source.record["args"]["connection_id"]
                    if port["direction"] == "input":
                        self.signal_inputs[scope, connection] = port["name"]
                    else:
                        ports.append({
                            "name": port["name"], "direction": "output", "use": "signal", "relation": "signal",
                            "signal": f"event/{connection}", "at": self.task_anchor(task),
                        })
                        self.submission(task)
                    continue
                side = "end" if port["direction"] == "output" or port.get("at_return", False) else "begin"
                span = self.device[scope] if port["use"] in {"device", "state"} else host
                tensor = port["tensor"]
                use = port["use"]
                if use in {"host", "device"}:
                    use = "data" if tensor is not None else "control"
                elif use == "state":

                    # Persistent buffer ports describe layout; state_accesses owns the actual read/write intervals.
                    use = "metadata"
                relation = "data" if use == "data" else "control"
                converted = {
                    "name": port["name"], "direction": port["direction"], "use": use,
                    "relation": relation, "at": self.endpoint(span, side),
                }
                if raw.get("asynchronous", False) and port["use"] == "device" and port["direction"] == "output":
                    converted["at"] = self.task_anchor(self.work_completions[scope])
                if tensor is not None:
                    converted["tensor"] = {name: tensor[name] for name in ("value", "storage", "version", "dtype", "shape", "writer", "allocation", "buffer")}
                    if "shape_rule" in tensor:
                        converted["tensor"]["shape_rule"] = tensor["shape_rule"]
                ports.append(converted)
                if tensor is not None and port["use"] not in {"state", "metadata"}:
                    observed.append((getattr(self.trace.scopes[scope], side), scope, converted))
        self.data_dependencies(observed)

    def data_dependencies(self, observed: list) -> None:

        # Only an already published version can feed another call; an enclosing writer's return is in the future.
        producers = {}
        for _, scope, port in sorted(observed, key=lambda item: item[0]):
            tensor = port["tensor"]
            key = tensor["storage"], tensor["version"]
            if port["direction"] == "output":
                producer, at = producers.setdefault(key, (scope, port["at"]))

                # A parent re-exports the child's ready value, not its own later completion. Siblings keep local ports.
                owner = producer
                while owner is not None and owner != scope:
                    owner = self.scopes[owner]["parent"]
                if owner == scope:
                    port["at"] = at
            elif key in producers:
                self.edge(producers[key][1], port["at"], "data")

    def task_anchor(self, task: StreamCommand, side: str = "end") -> dict[str, int]:
        stream = task.stream
        position = getattr(task, side)
        boundary = self.device_index.enclosing(Interval(position, position, stream, {}))
        if boundary is None:
            raise ValueError(f"required device endpoint has no semantic coverage: stream {stream} at {position}")
        return self.at(boundary.record["span"], position)

    def state_accesses(self) -> None:
        for scope, raw in enumerate(self.raw["scopes"]):
            for access in raw.get("state_accesses", []):
                spans = {"device": self.device, "call": self.host, "service": self.services}[access["interval"]]
                field = {"kv": "kv_accesses", "slot": "slot_accesses"}[access["space"]]
                self.scopes[scope][field].append({
                    **{name: value for name, value in access.items() if name != "interval"},
                    "start": self.endpoint(spans[scope], "begin"),
                    "finish": self.endpoint(spans[scope], "end"),
                })

    def posting(self, task: StreamCommand, *, closed: bool = False) -> tuple[Interval, int]:
        source = task.source
        api = source.api if isinstance(source, NativeStreamCall) else self.trace.apis[source.record["args"]["connection_id"]]
        if isinstance(source, NativeStreamCall) and not self.trace.tasks.get(api.record["args"]["connection_id"]):

            # A runtime-elided wait retains its logical condition without inventing a driver submission.
            return api, source.native.begin
        if closed:
            begin, _ = self.trace.queues.window(api, task.stream)
            return api, begin

        posts = self.trace.queues.inside(api, task.stream)
        if len(posts) != 1:
            raise ValueError(f"device command on stream {task.stream} needs a unique driver release: {api.record['name']}")
        return api, posts[0].begin

    def submission(self, task: StreamCommand, posting: tuple[Interval, int] | None = None) -> None:
        api, begin = self.posting(task) if posting is None else posting
        origin = self.trace.origin(api)
        source = task.source
        side = "end" if isinstance(source, NativeStreamCall) or source.record["args"]["Task Type"] == "EVENT_RECORD" else "begin"

        # Queue entry follows any CPU preparation inside the API; its outer entry is not a launch witness.
        launched = self.host_anchor(Interval(begin, begin, api.thread, {}))
        if origin is not api:

            # A separate enqueue is visible before the worker submits. A direct call enters the queue at the driver boundary.
            self.edge(self.host_anchor(origin), launched, "submission")
        self.edge(launched, self.task_anchor(task, side), "submission")

    def submission_tail(self, task: StreamCommand) -> None:
        at = self.task_anchor(task)
        span = self.spans[at["span"]]

        # Charge up to native payload completion, excluding the next command's queue residence.
        self.span(span["scope"], span["resource"], task.end, task.end, "boundary")

    def submissions(self) -> None:
        cuts: dict[int, set[int]] = defaultdict(set)
        for span in self.spans:
            if self.resources[span["resource"]]["kind"] == "stream":
                cuts[span["resource"]].update((span["begin"], span["end"]))
        for at in ObservationReferences({"scopes": self.scopes, "dependencies": self.dependencies}):
            resource = self.spans[at["span"]]["resource"]
            if resource in cuts:
                cuts[resource].add(at["order"])
        ordered = {resource: sorted(positions) for resource, positions in cuts.items()}

        prior, last = None, None
        for task in self.payload:
            enclosing = self.device_index.enclosing(Interval(task.begin, task.end, task.stream, {}))
            owner = enclosing.record["span"]
            span = self.spans[owner]
            resource = span["resource"]
            region = bisect_right(ordered[resource], task.begin) - 1

            # Closed collectives own their internal progress. Other work can start only at its actual queue post.
            closed = span["kind"] == "collective"
            if closed:
                release = ("collective", owner)
                posting = None
            else:
                posting = self.posting(task)
                release = ("driver", posting[0].thread, posting[1])
            key = resource, region, release
            if key != prior:
                if last is not None:
                    self.submission_tail(last)
                self.submission(task, self.posting(task, closed=True) if closed else posting)
                prior = key
            last = task

        if last is not None:
            self.submission_tail(last)

    def synchronization(self) -> None:
        syncs, waits = self.trace.host_waits, self.trace.stream_waits
        for api in self.trace.apis.values():
            if api.record["args"]["id"] not in syncs | waits:
                continue
            origin = self.trace.origin(api)
            owner = self.trace.owner(origin)
            if owner is None or owner in self.trace.closed_scopes or self.trace.excluded_ranges.enclosing(origin) is not None:
                continue
            native = [call for call in self.trace.runtime_ranges.overlapping(api) if call.record["api"] in syncs | waits]
            if not native:
                raise ValueError(f"required synchronization lacks native resource identity: {api.record['args']['id']}")

        # A child call or exposed endpoint starts a new region, even if its source run submits no device work.
        boundaries: dict[int, set[int]] = defaultdict(set)
        for span in self.spans:
            if self.resources[span["resource"]]["kind"] == "thread":
                boundaries[span["resource"]].update((span["begin"], span["end"]))
        anchors = [port["at"] for scope in self.scopes for port in scope["ports"]]
        anchors.extend(edge[side] for edge in self.dependencies for side in ("source", "target"))
        for at in anchors:
            resource = self.spans[at["span"]]["resource"]
            if resource in boundaries:
                boundaries[resource].add(at["order"])
        regions = {resource: sorted(points) for resource, points in boundaries.items()}
        previous_wait = None
        wait_names: dict[int, int] = defaultdict(int)
        for native, recorded in self.trace.synchronizations:
            row = native.record
            stream_wait = row["api"] in waits
            api = self.trace.native_api(native)
            origin = self.trace.origin(api)
            owner = self.trace.owner(origin)
            if owner is None or owner in self.trace.closed_scopes or self.trace.excluded_ranges.enclosing(origin) is not None:
                continue
            if stream_wait:
                waiting = self.streams.stream_calls[api.record["args"]["connection_id"]]
                completion = self.task_anchor(waiting)
                self.submission(waiting)
            else:
                completion = self.host_anchor(native, "end")

            signal = None
            if "Event" in row["api"]:
                previous_wait = None
                if recorded is None:
                    raise ValueError("required event wait has no observed recording")
                call = self.trace.native_api(recorded)
                tasks = [self.streams.task(task) for task in self.trace.tasks[call.record["args"]["connection_id"]]]
                signal = call.record["args"]["connection_id"]
            else:
                stream = None if row["api"] == "aclrtSynchronizeDeviceWithTimeout" else row["stream"]
                if stream is not None and row["stream_status"] != 0:
                    raise ValueError("required stream synchronization has no native stream identity")
                tasks = self.streams.submitted(native.begin, stream)

                # Keep the first target-frontier wait; repeated fences without new work add only CPU overhead.
                resource = self.spans[completion["span"]]["resource"]
                frontier = tuple(sorted((task.stream, task.end) for task in tasks))
                caller = self.host_anchor(origin)
                caller_resource = self.spans[caller["span"]]["resource"]
                region = bisect_right(regions[resource], native.begin), bisect_right(regions[caller_resource], origin.begin)
                key = owner, completion["span"], caller["span"], stream, region, frontier
                if key == previous_wait:
                    continue
                previous_wait = key

                # The source edge proves this run; the port preserves the operation for a different target frontier.
                peers = [self.resource("stream", stream, "execution")] if stream is not None else [
                    identity for identity, resource in enumerate(self.resources) if resource["kind"] == "stream"
                ]
                for peer in peers:
                    self.scopes[owner]["ports"].append({
                        "name": f"stream.wait/{wait_names[owner]}", "direction": "input", "use": "control",
                        "relation": "signal", "at": completion,
                        "wait": {"resource": peer, "begin": self.host_anchor(native)},
                    })
                    wait_names[owner] += 1
            if not stream_wait:
                resource = self.spans[completion["span"]]["resource"]
                self.span(owner, resource, native.begin, native.end, "wait")
            if (owner, signal) in self.signal_inputs:
                self.scopes[owner]["ports"].append({
                    "name": self.signal_inputs.pop((owner, signal)), "direction": "input", "use": "signal", "relation": "signal",
                    "signal": f"event/{signal}", "at": completion,
                })
            tails = {}
            for task in tasks:
                stream = task.stream
                if stream not in tails or task.end > tails[stream].end:
                    tails[stream] = task
            for stream, task in tails.items():

                # Payload submission belongs to its coarse interval, not to the fence's last internal kernel.
                if "Event" in row["api"]:
                    self.submission(task)
                self.edge(self.task_anchor(task), completion, "signal")

        if self.signal_inputs:
            raise ValueError(f"declared event waits have no native synchronization evidence: {self.signal_inputs}")

    def normalize_device_spans(self, result: dict) -> None:
        anchors = list(ObservationReferences(result))
        references = Counter(anchor["span"] for anchor in anchors)
        device = set(self.device.values())
        points = defaultdict(set)
        for identity, span in enumerate(self.spans):
            if identity not in device and self.resources[span["resource"]]["kind"] == "stream":
                points[span["resource"]].update((span["begin"], span["end"]))

        by_span = defaultdict(list)
        for anchor in {id(anchor): anchor for anchor in anchors}.values():
            identity, position = anchor["span"], anchor["order"]
            span = self.spans[identity]
            by_span[identity].append(anchor)

            # Interval endpoints come from observer records; interior native dependencies and explicit points remain.
            if self.resources[span["resource"]]["kind"] == "stream" and (
                identity not in device or span["begin"] < position < span["end"]
            ):
                points[span["resource"]].add(position)
        points = {resource: sorted(values) for resource, values in points.items()}
        for scope, span in self.device.items():
            references[span] -= sum(port["name"] in {"device.enter", "device.complete"} for port in self.scopes[scope]["ports"])

        discarded = set()
        for identity in device:
            span = self.spans[identity]
            begin, end = span["begin"], span["end"]
            positions = points.get(span["resource"], ())
            left, right = bisect_left(positions, begin), bisect_right(positions, end)
            bounds = [positions[left], positions[right - 1]] if left < right else []
            payload = self.resources[span["resource"]]["payload"]
            first = bisect_right(payload, begin, key=lambda interval: interval[1])
            last = bisect_left(payload, end, key=lambda interval: interval[0])
            if first < last:
                bounds.extend((max(begin, payload[first][0]), min(end, payload[last - 1][1])))
            if not bounds:
                if references[identity] == 0:
                    discarded.add(identity)
                continue

            # Semantic device lifetime covers native work and required fences, not the observer's enclosing markers.
            span["begin"], span["end"] = min(bounds), max(bounds)
            if span["begin"] == span["end"]:
                span["kind"] = "boundary"
            for anchor in by_span[identity]:
                if anchor["order"] == begin:
                    anchor["order"] = span["begin"]
                elif anchor["order"] == end:
                    anchor["order"] = span["end"]
        if not discarded:
            return

        for scope in self.scopes:
            scope["ports"][:] = [port for port in scope["ports"] if port["at"]["span"] not in discarded]

        # Publication ends interpretation: remap every surviving reference, including shared anchor objects, once.
        positions = {old: new for new, old in enumerate(index for index in range(len(self.spans)) if index not in discarded)}
        anchors = {id(anchor): anchor for anchor in ObservationReferences(result)}
        for anchor in anchors.values():
            anchor["span"] = positions[anchor["span"]]
        result["spans"] = [span for index, span in enumerate(self.spans) if index not in discarded]

    def host_service_positions(self, coordinates):
        trace = self.trace

        def wall_interval(interval):
            identity = f"thread/{interval.thread}"
            return interval.thread, trace.host_times[identity, interval.begin], trace.host_times[identity, interval.end]

        excluded = [wall_interval(interval) for interval in trace.excluded]
        for interval in trace.runtime:
            call = interval.record
            if call["api"] in trace.host_waits or (call["api"] == "aclrtMemcpy" and call["bytes"]):
                excluded.append(wall_interval(interval))
        dequeues = []
        for correlation, dequeue in trace.dequeue.items():
            enqueue = trace.enqueue[correlation]
            destination = excluded if trace.excluded_ranges.enclosing(enqueue) is not None else dequeues
            destination.append(wall_interval(dequeue))

        with self.activity_path.open() as lines:
            service = ThreadService((json.loads(line) for line in lines), dequeues, excluded)
        positions = defaultdict(list)
        for row in coordinates:
            lane = row["resource"]
            resource = self.resources[lane]
            if resource["kind"] != "thread":
                continue
            thread = int(resource["identity"].split("/")[1])
            observed = service.activity.get(thread)
            if observed is not None and observed[0] <= row["time_ns"] <= observed[1]:
                positions[lane, thread].append((row["order"], row["time_ns"]))

        # Differences of the prefix give the occupied service at any retained cut on the same thread.
        result = []
        for (lane, thread), cuts in positions.items():
            total = 0
            before = cuts[0][1]
            for order, timestamp in cuts:
                if timestamp < before:
                    raise ValueError("raw host cuts reverse execution order")
                if timestamp > before:
                    total += service.duration(thread, before, timestamp, thread not in self.control_threads)
                result.append([lane, order, total])
                before = timestamp
        return result

    def document(self, measurements: bool = False) -> dict[str, Any]:
        self.host_spans()
        self.service_spans()
        self.device_spans()
        self.collective_spans()
        self.call_ports()
        self.state_accesses()
        self.synchronization()

        # A true read observes completion without waiting; false reads add no producer dependency.
        for query in self.raw["state_queries"]:
            if query["kind"] == "event" and query["result"]:
                task = self.streams.task(self.trace.device_points[query["point"]].task)
                self.edge(self.task_anchor(task), self.endpoint(self.host[query["scope"]], "end"), "observation")

        self.submissions()

        # All producers declare payload; publish one union per resource before projection uses ordered ranges.
        for resource in self.resources:
            ranges = sorted(resource["payload"])
            resource["payload"] = []
            for begin, end in ranges:
                payload = resource["payload"]
                if payload and begin <= payload[-1][1]:
                    payload[-1][1] = max(payload[-1][1], end)
                elif begin < end:
                    payload.append([begin, end])
        result = {
            "environment": self.raw["environment"], "resources": self.resources, "scopes": self.scopes,
            "spans": self.spans, "dependencies": self.dependencies,
        }
        self.normalize_device_spans(result)
        if measurements:
            result["measurements"] = self.streams.measurements(result)
            if self.activity_path is not None:
                result["measurements"]["host_service_positions"] = self.host_service_positions(result["measurements"]["coordinates"])
            payloads = self.trace.collective_payloads({scope: row[-1] for scope, row in self.collectives.items()})
            result["measurements"]["collectives"] = [
                [scope, resource, begin, end, payloads.get(scope)]
                for scope, (resource, begin, end, _) in self.collectives.items()
            ]
            result["measurements"]["collective_peer_rows"] = {
                scope: self.raw["scopes"][scope]["peer_rows"] for scope in self.collectives
                if "peer_rows" in self.raw["scopes"][scope]
            }
            result["measurements"]["synchronous_transfers"] = self.transfer_times()
            result["native_effects"] = self.streams.evidence(self.resource_ids)
        return result
