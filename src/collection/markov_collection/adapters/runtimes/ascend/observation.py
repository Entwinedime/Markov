"""Turn native boundary evidence into the common coarse-projection input."""

from __future__ import annotations

from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.trace import AscendTrace, DeviceTask, Interval, StreamWait, ThreadIntervals


class AscendObservation:
    def __init__(self, directory: Path, rank: int, raw: dict, trace: Path) -> None:
        self.raw = raw
        self.rank = rank
        if raw["environment"]["parallel"]["rank"] != rank:
            raise ValueError("worker recording differs from the requested rank")
        profile, = (directory / "native" / f"rank-{rank}").glob("*_ascend_pt")
        native_points = {index for index, point in enumerate(raw["points"]) if point["side"] == "signal"}
        services = [scope for scope in raw["scopes"] if "service" in scope]
        self.trace = AscendTrace(profile, directory / "runtime", native_points, services, trace, raw["device_clock"])
        self.control_threads = {interval.thread for scope, interval in self.trace.scopes.items() if "service" not in raw["scopes"][scope]}
        self.resources = []
        self.resource_ids = {}
        self.spans = []
        self.dependencies = []
        self.host = {}
        self.device = {}
        self.services = {}
        self.payload = []
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
            if "communication" in scope:
                info["communication"] = scope["communication"]
            info["ports"] = []
            info["kv_accesses"] = []
            info["slot_accesses"] = []
            self.scopes.append(info)
        if set(self.trace.scopes) != set(range(1, len(self.scopes))):
            raise ValueError("native scope metadata and recorded CPU markers differ")

    def resource(self, kind: str, identity: int | str, role: str, *, serial: bool = True) -> int:
        key = kind, identity
        if key not in self.resource_ids:
            if kind == "thread":
                role = "control" if identity in self.control_threads else "submission"
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

        # Coordinates express ordering, not service time; observer calls must not fragment business CPU work.
        for scope, interval in self.trace.scopes.items():
            if self.raw["scopes"][scope]["thread"] != interval.thread:
                raise ValueError(f"scope {scope} differs from its recorded native thread")
            resource = self.resource("thread", interval.thread, "native")
            raw = self.raw["scopes"][scope]
            kind = "wait" if "service" in raw else "boundary" if raw.get("envelope", False) else "host"
            if "communication" in raw and "device_entry" not in raw:

                # A blocking CPU collective is communication work, not host compute plus a second copy of its wait.
                kind = "communication"
                if interval.begin < interval.end:
                    self.resources[resource]["payload"].append([interval.begin, interval.end])
            self.host[scope] = self.span(scope, resource, interval.begin, interval.end, kind)
        for resource in self.resources:
            resource["payload"].sort()
        self.host_index = self.span_index("thread")
        pending = None
        for dequeue in sorted(self.trace.dequeue.values(), key=lambda value: (value.thread, value.begin)):
            enqueue = self.trace.enqueue[dequeue.record["args"]["correlation_id"]]
            if self.trace.excluded_ranges.enclosing(enqueue) is not None:
                continue
            scope = self.trace.owner(enqueue)
            if scope is None:
                continue

            # Handle maintenance remains lifecycle evidence, not an extra submission segment between business tasks.
            apis = self.trace.api_ranges.inside(dequeue)
            if not any(self.trace.tasks.get(api.record["args"]["connection_id"]) or api.record["args"]["id"] in self.trace.host_waits | self.trace.stream_waits for api in apis):
                continue
            resource = self.resource("thread", dequeue.thread, "native")
            key = resource, scope
            if pending is not None and pending[0] == key:
                span = pending[1]
                self.spans[span]["end"] = dequeue.end
            else:
                span = self.span(scope, resource, dequeue.begin, dequeue.end, "host")
                self.edge(self.host_anchor(enqueue), self.endpoint(span, "begin"), "submission")
                pending = key, span
        self.host_index = self.span_index("thread")

    def service_spans(self) -> None:
        for scope, raw in enumerate(self.raw["scopes"]):
            if "service" not in raw:
                continue
            interval = self.trace.scopes[scope]
            service = raw["service"]
            resource = self.resource("service", interval.thread, service["role"], serial=False)
            span = self.span(scope, resource, interval.begin, interval.end, "boundary")
            self.services[scope] = span
            self.edge(self.endpoint(self.host[scope], "begin"), self.endpoint(span, "begin"), "submission")
            self.edge(self.endpoint(span, "end"), self.endpoint(self.host[scope], "end"), "signal")
            if "submitted_by" in raw:
                producer = self.host[raw["submitted_by"]]
                self.edge(self.endpoint(producer, "end"), self.endpoint(span, "begin"), "submission")

        # Each synchronous engine call owns its queue interval; outer orchestration is not a second transfer cost.
        for call in self.trace.services.executions:
            service = self.raw["scopes"][call.scope]["service"]
            resource = self.resource("service", f"queue/{call.stream}", service["role"])
            span = self.span(call.scope, resource, call.begin, call.end, service["kind"])
            outer = self.services[call.scope]
            self.edge(self.endpoint(outer, "begin"), self.endpoint(span, "begin"), "submission")
            self.edge(self.endpoint(span, "end"), self.endpoint(outer, "end"), "signal")
            if call.begin < call.end:
                self.resources[resource]["payload"].append([call.begin, call.end])

        for resource in self.resources:
            if resource["kind"] == "service":
                resource["payload"].sort()

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
            span = self.span(scope, resource, begin.order, end.order, kind)
            self.device[scope] = span

        # A native event may be recorded on another stream than its enclosing action's device interval.
        for identity, observed in enumerate(self.raw["points"]):
            if observed["side"] != "signal":
                continue
            point = points[identity]
            resource = self.resource("stream", point.stream, "execution")
            self.span(observed["scope"], resource, point.order, point.order, "boundary")

        self.device_index = self.span_index("stream")
        self.auxiliary_stream_spans()

        # Logical waits retain their own boundaries even when the SDK omits the physical task.
        for wait in self.trace.waits.values():
            resource = self.resource("stream", wait.stream, "execution")
            self.span(wait.scope, resource, wait.begin, wait.end, "wait")
        self.device_index = self.span_index("stream")
        self.resolve_work_completions()
        payload: dict[int, list[tuple[int, int]]] = defaultdict(list)
        for task in self.trace.payload:
            kind = task.record["args"]["Task Type"]
            if kind in {"EVENT_RECORD", "EVENT_WAIT", "PROFILING_ENABLE", "PROFILING_DISABLE"}:
                continue
            if task.service is not None or self.trace.auxiliary(task) or self.trace.outside_window(task):
                continue
            stream = int(task.record["args"]["Physic Stream Id"])
            resource = self.resource("stream", stream, "execution")
            if self.device_index.enclosing(Interval(task.begin, task.end, stream, {})) is None:
                raise ValueError(f"native payload has no semantic stream coverage: connection {task.record['args']['connection_id']}")
            self.payload.append(task)
            payload[resource].append((task.begin, task.end))
        for resource, intervals in payload.items():
            ranges = self.resources[resource]["payload"]
            for begin, end in sorted(intervals):
                if ranges and begin <= ranges[-1][1]:
                    ranges[-1][1] = max(ranges[-1][1], end)
                elif begin < end:
                    ranges.append([begin, end])

    def auxiliary_stream_spans(self) -> None:
        pending = None
        communications = []
        for task in sorted(self.trace.commands, key=lambda value: (value.stream, value.begin)):
            if isinstance(task, StreamWait):
                pending = None
                continue
            if task.service is not None or self.trace.auxiliary(task) or self.trace.outside_window(task):
                continue
            stream = int(task.record["args"]["Physic Stream Id"])
            if self.device_index.enclosing(Interval(task.begin, task.end, stream, {})) is not None:
                pending = None
                continue
            api = self.trace.apis.get(task.record["args"]["connection_id"])
            owner = None if api is None else self.trace.owner(self.trace.origin(api))
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
                    task = self.work_completions[port["producer"]] if "producer" in port else self.trace.device_points[port["point"]].task
                    connection = task.record["args"]["connection_id"]
                    if port["direction"] == "input":
                        self.signal_inputs[scope, connection] = port["name"]
                    else:
                        ports.append({
                            "name": port["name"], "direction": "output", "use": "signal", "relation": "signal",
                            "signal": f"event/{connection}", "at": self.task_anchor(task),
                        })
                        self.submission(task)
                    continue
                side = "begin" if port["direction"] == "input" else "end"
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

    def task_anchor(self, task: DeviceTask | StreamWait, side: str = "end") -> dict[str, int]:
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

    def submission(self, task: DeviceTask) -> None:
        api = self.trace.apis[task.record["args"]["connection_id"]]
        origin = self.trace.origin(api)
        side = "end" if task.record["args"]["Task Type"] == "EVENT_RECORD" else "begin"

        # A completion exposed to the CPU also needs its actual submission; internal kernels stay encapsulated.
        launched = self.host_anchor(api)
        self.edge(self.host_anchor(origin), launched, "submission")
        self.edge(launched, self.task_anchor(task, side), "submission")

    def submissions(self) -> None:
        cuts: dict[int, set[int]] = defaultdict(set)
        for span in self.spans:
            if self.resources[span["resource"]]["kind"] == "stream":
                cuts[span["resource"]].update((span["begin"], span["end"]))
        for dependency in self.dependencies:
            for side in ("source", "target"):
                at = dependency[side]
                resource = self.spans[at["span"]]["resource"]
                if resource in cuts:
                    cuts[resource].add(at["order"])
        ordered = {resource: sorted(positions) for resource, positions in cuts.items()}
        groups = {}
        for task in self.payload:
            stream = int(task.record["args"]["Physic Stream Id"])
            resource = self.resource_ids["stream", stream]
            key = resource, bisect_right(ordered[resource], task.begin) - 1
            if key not in groups:
                groups[key] = task, task
            else:
                first, last = groups[key]
                groups[key] = min(first, task, key=lambda value: value.begin), max(last, task, key=lambda value: value.end)
        for (resource, _), (first, last) in groups.items():
            self.submission(first)
            at = self.task_anchor(last)
            scope = self.spans[at["span"]]["scope"]

            # Device work ends at native payload, without a dependency on the observer's later return marker.
            self.span(scope, resource, last.end, last.end, "boundary")

    def synchronization(self) -> None:
        syncs, waits = self.trace.host_waits, self.trace.stream_waits
        for api in self.trace.apis.values():
            if api.record["args"]["id"] not in syncs | waits:
                continue
            origin = self.trace.origin(api)
            if self.trace.owner(origin) is None or self.trace.excluded_ranges.enclosing(origin) is not None:
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
            if owner is None or self.trace.excluded_ranges.enclosing(origin) is not None:
                continue
            host = self.host_anchor(api, "end")
            signal = None
            if "Event" in row["api"]:
                previous_wait = None
                if recorded is None:
                    raise ValueError("required event wait has no observed recording")
                call = self.trace.native_api(recorded)
                tasks = self.trace.tasks[call.record["args"]["connection_id"]]
                signal = call.record["args"]["connection_id"]
            else:
                stream = None if row["api"] == "aclrtSynchronizeDeviceWithTimeout" else row["stream"]
                if stream is not None and row["stream_status"] != 0:
                    raise ValueError("required stream synchronization has no native stream identity")
                tasks = self.trace.submitted(api.begin, stream)

                # Keep the first target-frontier wait; repeated fences without new work add only CPU overhead.
                resource = self.spans[host["span"]]["resource"]
                frontier = tuple(sorted((task.stream, task.end) for task in tasks))
                caller = self.host_anchor(origin)
                caller_resource = self.spans[caller["span"]]["resource"]
                region = bisect_right(regions[resource], api.begin), bisect_right(regions[caller_resource], origin.begin)
                key = owner, host["span"], caller["span"], stream, region, frontier
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
                        "relation": "signal", "at": host,
                        "wait": {"resource": peer, "begin": self.host_anchor(api)},
                    })
                    wait_names[owner] += 1
            if not stream_wait:
                resource = self.spans[host["span"]]["resource"]
                self.span(owner, resource, api.begin, api.end, "wait")
            if stream_wait:
                waiting = self.trace.waits[api.record["args"]["connection_id"]]
                host = self.task_anchor(waiting)
                launched = self.host_anchor(api)
                self.edge(self.host_anchor(origin), launched, "submission")
                self.edge(launched, host, "submission")
            if (owner, signal) in self.signal_inputs:
                self.scopes[owner]["ports"].append({
                    "name": self.signal_inputs.pop((owner, signal)), "direction": "input", "use": "signal", "relation": "signal",
                    "signal": f"event/{signal}", "at": host,
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
                self.edge(self.task_anchor(task), host, "signal")

        if self.signal_inputs:
            raise ValueError(f"declared event waits have no native synchronization evidence: {self.signal_inputs}")

    def discard_unused_markers(self, result: dict) -> None:
        references = Counter(anchor["span"] for anchor in self.trace.anchors(result))
        for scope, span in self.device.items():

            # Generated enter/complete ports alone do not establish a business use of this stream interval.
            references[span] -= sum(port["name"] in {"device.enter", "device.complete"} for port in self.scopes[scope]["ports"])
        discarded = set()
        for identity in self.device.values():
            span = self.spans[identity]
            payload = self.resources[span["resource"]]["payload"]
            previous = bisect_left(payload, span["end"], key=lambda interval: interval[0]) - 1
            occupied = previous >= 0 and payload[previous][1] > span["begin"]
            if references[identity] == 0 and not occupied:
                discarded.add(identity)
        if not discarded:
            return

        for scope in self.scopes:
            scope["ports"][:] = [port for port in scope["ports"] if port["at"]["span"] not in discarded]

        # Publication ends interpretation: remap every surviving reference, including shared anchor objects, once.
        positions = {old: new for new, old in enumerate(index for index in range(len(self.spans)) if index not in discarded)}
        anchors = {id(anchor): anchor for anchor in self.trace.anchors(result)}
        for anchor in anchors.values():
            anchor["span"] = positions[anchor["span"]]
        result["spans"] = [span for index, span in enumerate(self.spans) if index not in discarded]

    def document(self, measurements: bool = False) -> dict[str, Any]:
        self.host_spans()
        self.service_spans()
        self.device_spans()
        self.call_ports()
        self.state_accesses()
        self.synchronization()
        self.submissions()
        result = {
            "environment": self.raw["environment"], "resources": self.resources, "scopes": self.scopes,
            "spans": self.spans, "dependencies": self.dependencies,
        }
        self.discard_unused_markers(result)
        if measurements:
            result["measurements"] = self.trace.measurements(result)
        return result
