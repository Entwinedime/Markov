"""Check retained host, state and signal boundaries against original native observations."""

from collections import Counter, defaultdict
from decimal import Decimal
import json

from markov_collection.adapters.runtimes.ascend.clock import AscendClock, HostCounter
from markov_collection.adapters.runtimes.ascend.trace import AscendTrace, Interval, ThreadIntervals


class NativeEffectEvidence:
    task_fields = ("connection_id", "Batch Id", "Task Id", "Subtask Id")
    waits = {"aclrtStreamWaitEvent", "aclrtStreamWaitEventWithTimeout"}

    def __init__(self, capture, rank, native, facts, sdk):
        self.native, self.facts = native, facts
        self.logical_coordinates = set()
        profile, = (capture / "native" / f"rank-{rank}").glob("*_ascend_pt")
        counter = HostCounter(profile)
        device, = profile.glob("PROF_*/device_*")
        self.clock = AscendClock(device, native["device_clock"])
        flips = AscendTrace.task_flips(profile)
        rows = json.loads(sdk.read_text(), parse_float=Decimal)
        self.apis = {}
        self.tasks = defaultdict(list)
        self.task_records = {}
        self.enqueues = {}
        self.dequeue_ids = {}
        self.host_measurements = {}
        dequeues = []
        for row in rows:
            if row.get("ph") != "X":
                continue
            args = row.get("args", {})
            if "Task Type" not in args and (row.get("cat") in {"enqueue", "dequeue"} or "connection_id" in args):
                interval = Interval.read(row, counter)

                # Reconstruct expected times from raw SDK coordinates, never from interpreted measurements.
                for position in (interval.begin, interval.end):
                    self.host_measurements[f"thread/{interval.thread}", position] = (
                        counter.framework_raw + round(Decimal(position - counter.anchor) * 1_000_000_000 / counter.frequency)
                    )
            if "Task Type" in args:
                if args["Task Type"] == "PLACE_HOLDER_SQE" and (
                    int(args["Physic Stream Id"]), int(args["Task Id"]), int(args["Batch Id"])
                ) in flips:
                    continue
                self.tasks[args["connection_id"]].append(args)
                key = int(args["Physic Stream Id"]), self.task_key(args)
                self.require(key not in self.task_records, "raw SDK repeats a physical task identity")
                self.task_records[key] = row
            elif row.get("cat") in {"enqueue", "dequeue"}:
                if row["cat"] == "enqueue":
                    self.enqueues[args["correlation_id"]] = interval
                else:
                    dequeues.append(interval)
                    self.dequeue_ids[args["correlation_id"]] = interval
            elif "Thread Id" in args and "connection_id" in args:
                self.require(args["connection_id"] not in self.apis, "raw SDK repeats an API connection")
                self.apis[args["connection_id"]] = interval
        self.api_ranges = ThreadIntervals(list(self.apis.values()))
        self.dequeues = ThreadIntervals(dequeues)
        self.enqueue_ranges = ThreadIntervals(list(self.enqueues.values()))

        scopes, excluded, windows = [], [], []
        self.host_records = {}
        self.points = {}
        signals = {index for index, point in enumerate(native["points"]) if point["side"] == "signal"}
        for row in native["host_observation"]["intervals"]:
            interval = self.interval(row)
            name = row["name"]
            if name == "markov.window":
                windows.append(interval)
            elif name.startswith("markov.scope/"):
                scopes.append(interval)
                identity = int(name.split("/")[1])
                self.require(identity not in self.host_records, "raw recording repeats a host scope")
                self.host_records[identity] = row
            elif name.startswith("markov.point/"):
                identity = int(name.split("/")[1])
                self.require(identity not in self.points, "raw recording repeats a device point")
                self.points[identity] = interval
                if identity not in signals:
                    excluded.append(interval)
            elif name in {"markov.observer", "markov.auxiliary"}:
                excluded.append(interval)
        self.window, = windows
        for scope in native["scopes"]:
            if "service" in scope:
                self.require(scope["id"] not in self.host_records, "native service has competing host records")
                self.host_records[scope["id"]] = {**scope["host_interval"], "thread": scope["thread"]}
        self.require(set(self.host_records) == set(range(1, len(native["scopes"]))), "raw host scope observations are incomplete")
        self.scopes, self.excluded = ThreadIntervals(scopes), ThreadIntervals(excluded)
        process = native["host_observation"]["process"]
        self.runtime = [json.loads(line) for line in (capture / "runtime" / f"{process}.jsonl").read_text().splitlines()]
        self.runtime_records = {(row["thread"], row["begin_counter"]): row for row in self.runtime}
        self.require(len(self.runtime_records) == len(self.runtime), "raw runtime repeats an API interval identity")
        self.source_scopes = {origin: index for index, scope in enumerate(facts["scopes"])
                              for origin in scope["bindings"]["source_scopes"]}
        self.closed_scopes = set()
        for scope in native["scopes"]:
            if "service" in scope or scope["parent"] in self.closed_scopes:
                self.closed_scopes.add(scope["id"])

        # Resolve actual queue generations independently of the production command interpreter.
        queues, submissions = {}, defaultdict(list)
        self.driver_submissions = []
        for row in sorted(self.runtime, key=lambda row: row["begin_counter"]):
            if not row["api"].startswith("halSq"):
                continue
            self.require(row["result"] == 0, "raw driver operation failed")
            key = row["device"], row["task_system"], row["queue"]
            if row["api"] == "halSqCqAllocate":
                queues[key] = row["stream"]
            elif row["api"] == "halSqCqFree":
                del queues[key]
            elif row["api"] == "halSqTaskSend":
                stream = queues[key]
                submissions[stream].append(self.interval(row))
                self.driver_submissions.append((stream, row))
        self.submissions = {stream: ThreadIntervals(calls) for stream, calls in submissions.items()}

        # Actual RAW samples override SDK reconstruction at their own coordinates, including service threads.
        services = [{**scope["host_interval"], "thread": scope["thread"]}
                    for scope in native["scopes"] if "service" in scope]
        self.queries = {query["scope"]: {**query, "thread": native["scopes"][query["scope"]]["thread"]}
                        for query in native["state_queries"]}
        for row in [*native["host_observation"]["intervals"], *services, *self.runtime, *self.queries.values()]:
            for side in ("begin", "end"):
                for resource in (f"thread/{row['thread']}", f"service/{row['thread']}"):
                    self.host_measurements[resource, row[f"{side}_counter"]] = row[f"{side}_raw_ns"]

    @staticmethod
    def require(condition, message):
        if not condition:
            raise ValueError(message)

    @staticmethod
    def interval(row):
        return Interval(row["begin_counter"], row["end_counter"], row["thread"], row)

    def owner(self, api):
        dequeue = self.dequeues.enclosing(api)
        origin = self.enqueues[dequeue.record["args"]["correlation_id"]] if dequeue else api
        scope = self.scopes.enclosing(origin)
        if scope is None or self.excluded.enclosing(origin) is not None:
            return None
        return int(scope.record["name"].split("/")[1])

    def submissions_inside(self, interval, stream):
        index = self.submissions.get(stream)
        return index.inside(interval) if index else []

    def task_key(self, task):
        return tuple(task[name] for name in self.task_fields)

    def inspect_measurements(self):
        checked = 0
        unmeasured = set()
        for point in self.facts["measurements"]["coordinates"]:
            resource = self.facts["resources"][point["resource"]]
            if resource["kind"] == "stream":
                if point["time_ns"] is None:
                    unmeasured.add((point["resource"], point["order"]))
                continue
            key = resource["identity"], point["order"]
            self.require(key in self.host_measurements, f"host boundary has no original time source: {key}")
            self.require(point["time_ns"] == self.host_measurements[key], f"host measurement is missing or changes its original time: {key}")
            checked += 1

        # inspect() independently checks these native commands against raw API and SDK task records.
        native_only = {(effect["resource"], command[side])
                       for effect in self.facts["native_effects"] for command in effect["commands"]
                       if command["sdk"] is None for side in ("begin", "end")}
        self.require(unmeasured <= native_only, "device boundary loses a recorded timestamp or lacks an observed native command")
        return {"checked_host_measurements": checked, "unmeasured_device_endpoints": len(unmeasured - self.logical_coordinates),
                "logical_wait_boundaries": len(unmeasured & self.logical_coordinates)}

    def inspect_boundaries(self, graph):
        spans = Counter((span["scope"], span["resource"], span["begin"], span["end"], span["kind"]) for span in self.facts["spans"])
        resources = {(resource["kind"], resource["identity"]): index for index, resource in enumerate(graph["resources"])}
        self.require(len(resources) == len(graph["resources"]), "coarse graph repeats a native resource")

        # SDK ownership is fixed for this recording, so CPU regions share one device-owner set.
        device_owners = {self.owner(api) for connection, api in self.apis.items() if self.tasks.get(connection)}
        counts = Counter()
        for index, scope in enumerate(graph["scopes"]):
            origins = scope["bindings"]["source_scopes"]
            if origins == [0]:
                continue
            records = [self.host_records[origin] for origin in origins]
            raw = self.native["scopes"][origins[0]]
            thread = raw["thread"]
            self.require(all(record["thread"] == thread for record in records), f"scope {index} changes its native caller thread")
            begin = min(records, key=lambda record: record["begin_counter"])
            end = max(records, key=lambda record: record["end_counter"])
            ports = {port["name"]: port for port in scope["ports"]}
            kind = "wait" if "service" in raw else "boundary" if raw.get("envelope", False) else "host"
            if "communication" in raw and "device_entry" not in raw:
                kind = "communication"
            host = resources["thread", f"thread/{thread}"]
            if origins[0] in self.queries:
                self.require(len(origins) == 1 and spans[index, host, begin["begin_counter"], end["end_counter"], "boundary"] == 1,
                             f"query {index} loses its uncharged caller envelope")
                read = self.queries[origins[0]]
                self.require(begin["begin_counter"] <= read["begin_counter"] <= read["end_counter"] <= end["end_counter"],
                             f"query {index} escapes its original caller interval")
                self.check_boundary(graph, ports["caller.return"]["event"], host, end, "end", index)
                counts["caller_boundaries"] += 1
                begin = end = read
                kind = "host"
            self.require(spans[index, host, begin["begin_counter"], end["end_counter"], kind] == 1,
                         f"scope {index} omits, duplicates or changes its native host interval")
            for name, side, record in (("call.enter", "begin", begin), ("call.return", "end", end)):
                self.check_boundary(graph, ports[name]["event"], host, record, side, index)
                counts["host_boundaries"] += 1
            counts["host_intervals"] += 1
            if scope["role"] == "segment" and len(origins) > 1:

                # A CPU-only region cannot conceal real SDK work, even if its semantic declarations were incomplete.
                self.require(device_owners.isdisjoint(origins), "closed CPU region hides native device work")
                counts["closed_cpu_regions"] += 1
            if "service" in raw:
                service = resources["service", f"service/{thread}"]
                self.require(not graph["resources"][service]["serial"], "synchronous service is charged as a serial caller resource")
                self.require(spans[index, service, begin["begin_counter"], end["end_counter"], raw["service"]["kind"]] == 1,
                             f"scope {index} omits, duplicates or changes its native service interval")
                self.check_boundary(graph, ports["service.complete"]["event"], service, end, "end", index)
                counts["service_intervals"] += 1
            if "native_transfer" in raw:

                # Each payload keeps its own readiness interval; merging KV and later metadata invents early reads.
                for access in scope["kv_accesses"] + scope["slot_accesses"]:
                    self.check_boundary(graph, access["start"], service, begin, "begin", index)
                    self.check_boundary(graph, access["finish"], service, end, "end", index)
                counts["derived_state_accesses"] += len(scope["kv_accesses"]) + len(scope["slot_accesses"])
            else:
                counts["declared_state_accesses"] += sum(len(self.native["scopes"][origin].get("state_accesses", ())) for origin in origins)
        return dict(counts)

    def inspect_services(self):
        services = {scope["id"]: scope for scope in self.native["scopes"] if "service" in scope}
        expected = Counter()
        observed = set()

        # Reconstruct ownership from original submissions, without reading the producer's task assignments.
        for stream, row in self.driver_submissions:
            if row["service"] < 0:
                continue
            scope = services[row["service"]]
            matches = [index for index, call in enumerate(scope["service"]["executions"])
                       if call["thread"] == row["thread"] and call["begin_counter"] <= row["begin_counter"]
                       and row["end_counter"] <= call["end_counter"]]
            self.require(len(matches) == 1, "private submission has no unique declared synchronous execution")
            self.require(type(row["tasks"]) is int and row["tasks"] >= 0, "private submission has no valid observed SQE count")
            expected[stream] += row["tasks"]
            observed.add((scope["id"], matches[0]))

        declared = {(scope["id"], index) for scope in services.values() for index in range(len(scope["service"]["executions"]))}
        self.require(observed == declared, "synchronous service executions lack raw queue evidence")
        self.require(not any(stream in expected and row["service"] < 0 for stream, row in self.driver_submissions),
                     "private queue has unowned work outside its closed services")
        for stream, count in expected.items():
            self.require(sum(key[0] == stream for key in self.task_records) == count,
                         "private queue SDK coverage differs from its actual submitted SQE counts")

        return {"synchronous_services": len(services), "executions": len(declared), "private_queues": len(expected),
                "submitted_tasks": sum(expected.values())}

    def inspect_submissions(self):
        submitted = {(row["thread"], row["begin_counter"]): stream for stream, row in self.driver_submissions}
        logical = set()
        for effect in self.facts["native_effects"]:
            row = self.runtime_records[effect["thread"], effect["begin_counter"]]
            if row["api"] in self.waits:
                for command in effect["commands"]:
                    if command["sdk"] is None:
                        logical.add((effect["resource"], command["end"], row["thread"], row["begin_counter"]))

        counts = Counter()
        for edge in self.facts["dependencies"]:
            source, target = edge["source"], edge["target"]
            resources = [self.facts["resources"][self.facts["spans"][point["span"]]["resource"]] for point in (source, target)]
            if edge["kind"] != "submission" or (resources[0]["kind"], resources[1]["kind"]) != ("thread", "stream"):
                continue
            thread = int(resources[0]["identity"].split("/")[1])
            stream = int(resources[1]["identity"].split("/")[1])
            resource = self.facts["spans"][target["span"]]["resource"]
            if (resource, target["order"], thread, source["order"]) in logical:
                counts["logical_waits"] += 1
                continue
            self.require(submitted.get((thread, source["order"])) == stream,
                         "device submission uses an API entry or a different queue instead of an actual driver boundary")
            counts["driver_boundaries"] += 1
        return dict(counts)

    def check_boundary(self, graph, event, resource, record, side, scope):
        measured = graph["evidence"]["events"][event]
        self.require(graph["events"][event]["resource"] == resource
                     and measured["order"] == record[side + "_counter"] and measured["time_ns"] == record[side + "_raw_ns"],
                     f"scope {scope} changes its native {side} boundary or measurement")

    def native_api(self, interval):
        matches = [api for api in self.api_ranges.inside(interval) if api.record["args"]["id"] == interval.record["api"]]
        self.require(len(matches) == 1, "raw stream operation lacks one SDK API")
        return matches[0]

    def point_connection(self, point):
        enqueue, = self.enqueue_ranges.inside(self.points[point])
        dequeue = self.dequeue_ids[enqueue.record["args"]["correlation_id"]]
        api, = [api for api in self.api_ranges.inside(dequeue) if api.record["args"]["id"] == "aclrtRecordEvent"]
        return api.record["args"]["connection_id"]

    def check_device_end(self, graph, event, connection, kind):
        task, = self.tasks[connection]
        self.require(task["Task Type"] == kind, "native signal has a different SDK task kind")
        resource = graph["resources"][graph["events"][event]["resource"]]
        self.require((resource["kind"], resource["identity"]) == ("stream", f"stream/{int(task['Physic Stream Id'])}"),
                     "native signal changes its stream")
        measured = self.clock.interval(self.task_records[int(task["Physic Stream Id"]), self.task_key(task)])[1]
        self.require(graph["evidence"]["events"][event]["time_ns"] == measured, "native signal changes its SDK completion measurement")

    def event_generations(self):
        peers = {
            scope["id"]: {port["producer"] for port in scope["ports"]
                          if port["use"] == "signal" and port["direction"] == "input" and "producer" in port}
            for scope in self.native["scopes"]
        }
        records, work, waits = {}, {}, defaultdict(list)
        for row in sorted(self.runtime, key=lambda row: row["begin_counter"]):
            if row["api"] == "aclrtRecordEvent":
                records[row["event"]] = row
            elif row["api"] == "aclrtDestroyEvent":
                records.pop(row["event"], None)
            elif row["api"] in self.waits | {"aclrtSynchronizeEvent", "aclrtSynchronizeEventWithTimeout"}:
                interval = self.interval(row)
                if interval.end <= self.window.begin or self.window.end <= interval.begin:
                    continue
                api = self.native_api(interval)
                owner = self.owner(api)
                if owner is None:
                    continue
                producer = records.get(row["event"])
                self.require(producer is not None, "native event consumer has no recorded generation")
                recorded = self.native_api(self.interval(producer))
                connection = recorded.record["args"]["connection_id"]
                waits[owner, connection].append((row, api))
                producer = self.owner(recorded)
                if producer in peers[owner]:
                    self.require(work.setdefault(producer, connection) == connection, "native Work has competing completion generations")
        return work, waits

    def wait_matches(self, graph, event, choices, effects):
        resource = graph["events"][event]["resource"]
        position = graph["evidence"]["events"][event]["order"]
        for row, api in choices:
            if row["api"] in self.waits:
                effect = effects[row["thread"], row["begin_counter"]]
                command, = effect["commands"]
                if resource == effect["resource"] and position == command["end"]:
                    return True
            else:
                descriptor = graph["resources"][resource]
                if (descriptor["kind"], descriptor["identity"]) == ("thread", f"thread/{api.thread}") and position == api.end:
                    return True
        return False

    def inspect_signals(self, contents, dependencies):
        graph, counts = contents.graph, Counter()
        work, waits = self.event_generations()

        producers, consumers = defaultdict(set), []
        effects = {(effect["thread"], effect["begin_counter"]): effect for effect in self.facts["native_effects"]}
        for raw in self.native["scopes"][1:]:
            index = contents.source_ids[raw["id"]]
            ports = {(port["name"], port["direction"]): port for port in graph["scopes"][index]["ports"]}
            for port in raw["ports"]:
                if port["use"] != "signal":
                    continue
                observed = ports[port["name"], port["direction"]]
                self.require(observed["use"] == "signal" and observed["relation"] == "signal", "native signal loses its port contract")
                event = observed["event"]
                if "boundary" in port:
                    boundary = port["boundary"]
                    reference = graph["scopes"][contents.source_ids[boundary["scope"]]]
                    names = {("call", "begin"): "call.enter", ("call", "end"): "call.return", ("device", "begin"): "device.enter",
                             ("device", "end"): "device.complete", ("service", "end"): "service.complete"}
                    name = names[boundary["interval"], boundary["side"]]
                    reference, = [candidate for candidate in reference["ports"] if candidate["name"] == name]
                    self.require(event == reference["event"], "native boundary signal changes its referenced endpoint")
                    counts["boundary_signals"] += 1
                    continue
                connection = self.point_connection(port["point"]) if "point" in port else work[port["producer"]]
                self.require(observed["signal"] == f"event/{connection}", "native signal changes its completion generation")
                if port["direction"] == "output":
                    self.check_device_end(graph, event, connection, "EVENT_RECORD")
                    producers[connection].add(event)
                    counts["exported_completions"] += 1
                    continue

                # A wait consumes its actual generation at the CPU API return or stream-wait completion.
                self.require(self.wait_matches(graph, event, waits[raw["id"], connection], effects),
                             "native signal input is detached from its actual wait completion")
                consumers.append((connection, event))
                counts["signal_waits"] += 1

        for query in self.native["state_queries"]:
            if query["kind"] == "event" and query["result"]:
                scope = graph["scopes"][contents.source_ids[query["scope"]]]
                event, = [port["event"] for port in scope["ports"] if port["name"] == "call.return"]
                consumers.append((self.point_connection(query["point"]), event))
                counts["true_completion_reads"] += 1
        for connection, event in consumers:
            self.require(producers[connection] and any(dependencies.reaches(producer, event) for producer in producers[connection]),
                         "native completion consumer has no path from its actual producer")
        return dict(counts)

    def inspect(self, graph):
        effects = self.facts["native_effects"]
        recorded = {(effect["thread"], effect["begin_counter"]): effect for effect in effects}
        self.require(len(recorded) == len(effects), "native effects repeat an API instance")
        expected, coordinates, counts = set(), {}, Counter()
        spans = defaultdict(list)
        for span in self.facts["spans"]:
            spans[span["resource"]].append(span)
        for row in self.runtime:
            if row["api"] == "aclrtMemcpy" and row["bytes"] > 0:
                interval = self.interval(row)
                if self.window.begin <= interval.begin and interval.end <= self.window.end:
                    owner = self.owner(self.native_api(interval))
                    if owner is not None and owner not in self.closed_scopes:
                        self.inspect_transfer(owner, row, spans, graph)
                        counts["synchronous_copies"] += 1
            copy = row["api"] == "aclrtMemcpyAsync" and row["bytes"] > 0
            if not copy and row["api"] not in self.waits:
                continue
            interval = self.interval(row)
            if interval.end <= self.window.begin or self.window.end <= interval.begin:
                continue
            api = self.native_api(interval)
            owner = self.owner(api)
            if owner is None or owner in self.closed_scopes:
                continue
            key = row["thread"], row["begin_counter"]
            expected.add(key)
            self.require(key in recorded, f"native stream effect is omitted: {key}, {row['api']}")
            effect = recorded[key]
            connection, stream = api.record["args"]["connection_id"], row["stream"]
            self.require(row["result"] == 0 and row["stream_status"] == 0, "raw stream operation failed or has no stream identity")
            self.require((effect["source_scope"], effect["connection"]) == (owner, connection), "native effect changes its API or owner")
            resource = self.facts["resources"][effect["resource"]]
            tasks = self.tasks.get(connection, [])
            if copy and not tasks:
                submitted = self.submissions_inside(interval, stream)
                candidates = []
                for parent in self.api_ranges.overlapping(api) if submitted else []:
                    group = self.tasks.get(parent.record["args"]["connection_id"], [])
                    if group and parent.contains(api) and self.owner(parent) == owner and self.submissions_inside(parent, stream) == submitted:
                        candidates.append(group)
                self.require(len(candidates) <= 1, "native copy has ambiguous enclosing device work")
                tasks = candidates[0] if candidates else []
                counts["enclosing_group_copies" if tasks else "host_completed_copies"] += 1
            witness = effect["completion"]
            completed = None
            if copy and not tasks:
                self.require(witness is not None, "unmatched copy has no raw completion witness")
                completed = self.runtime_records[witness["thread"], witness["begin_counter"]]
                self.require(completed["api"] == witness["api"] and completed["api"] in {"drvMemcpy", "halMemcpy"}
                             and completed["thread"] == row["thread"] and completed["result"] == 0
                             and row["begin_counter"] <= completed["begin_counter"] <= completed["end_counter"] <= row["end_counter"]
                             and all(completed[key] == row[key] for key in ("source", "destination", "bytes")),
                             "copy completion does not witness a synchronous transfer of the actual buffers")
            else:
                self.require(witness is None, "queued work or stream wait claims an unrelated host-copy completion")
            if completed is not None:
                host = self.inspect_transfer(owner, completed, spans, graph)
                self.require(effect["resource"] == host and not effect["commands"],
                             "synchronous transfer invents a stream command or changes its actual caller")
                counts["copies"] += 1
                continue
            self.require((resource["kind"], resource["identity"]) == ("stream", f"stream/{stream}"), "native effect changes stream")
            self.require(all(int(task["Physic Stream Id"]) == stream for task in tasks), "native effect's SDK tasks change stream")
            expected_tasks = Counter(self.task_key(task) for task in tasks) if tasks else Counter({None: 1})
            self.require(Counter(self.task_key(command["sdk"]) if command["sdk"] is not None else None
                                 for command in effect["commands"]) == expected_tasks, "native effect duplicates, omits or invents device work")

            if not copy and not tasks:
                self.require(all(submission.record["tasks"] == 0 for submission in self.submissions_inside(interval, stream)),
                             "unmatched stream wait has actual driver submissions")
                self.logical_coordinates.update((effect["resource"], command[side])
                                                for command in effect["commands"] for side in ("begin", "end"))
                counts["elided_stream_waits"] += 1

            for command in effect["commands"]:
                begin, end = command["begin"], command["end"]
                self.require(begin < end and any(span["begin"] <= begin < end <= span["end"] for span in spans[effect["resource"]]),
                             "native effect has no retained stream span")
                if copy:
                    self.require(any(first <= begin < end <= last for first, last in resource["payload"]), "native copy is absent from work payload")
                else:
                    self.require(any(span["kind"] == "wait" and span["begin"] == begin and span["end"] == end
                                     for span in spans[effect["resource"]]), "native stream wait is absent")
                if command["sdk"] is not None:
                    identity = stream, self.task_key(command["sdk"])
                    self.require(coordinates.setdefault(identity, (begin, end)) == (begin, end), "nested API views duplicate a device task")
            counts["copies" if copy else "stream_waits"] += 1
        self.require(set(recorded) == expected, "effect evidence contains an unobserved or auxiliary operation")
        self.check_sdk_order(coordinates)
        return dict(counts)

    def inspect_transfer(self, source_scope, completed, spans, graph):
        scope = self.source_scopes[source_scope]
        resources = self.facts["resources"]
        host, = [index for index, resource in enumerate(resources)
                 if (resource["kind"], resource["identity"]) == ("thread", f"thread/{completed['thread']}")]
        begin, end = completed["begin_counter"], completed["end_counter"]
        self.require(any(span["scope"] == scope and span["kind"] == "transfer"
                         and (span["begin"], span["end"]) == (begin, end) for span in spans[host]),
                     "synchronous copy loses its caller transfer interval")
        self.require(any(first <= begin < end <= last for first, last in resources[host]["payload"]),
                     "synchronous transfer is absent from its caller payload")
        raw = self.native["scopes"][source_scope]
        while "device_entry" not in raw and raw["parent"] is not None:
            raw = self.native["scopes"][raw["parent"]]
        if "device_entry" not in raw:
            return host

        owner = self.source_scopes[raw["id"]]
        port, = [port for port in self.facts["scopes"][owner]["ports"] if port["name"] == "device.complete"]
        published, = [port for port in graph["scopes"][owner]["ports"] if port["name"] == "device.complete"]
        self.check_device_end(graph, published["event"], self.point_connection(raw["device_return"]), "EVENT_RECORD")
        self.require(any(edge["kind"] == "signal" and edge["target"] == port["at"]
                         and self.facts["spans"][edge["source"]["span"]]["resource"] == host and edge["source"]["order"] == end
                         for edge in self.facts["dependencies"]),
                     "synchronous transfer has no readiness edge to its actual enclosing return fence")
        return host

    def check_sdk_order(self, coordinates):
        ordered = []
        for (stream, task), (begin, end) in coordinates.items():
            row = self.task_records[stream, task]
            ordered.append((stream, row["ts"], task[1:], begin, end))

        # Only retained SDK identities are compared; unknown private operations need no invented pairing.
        ordered.sort()
        for left, right in zip(ordered, ordered[1:]):
            self.require(left[0] != right[0] or left[-1] <= right[-2], "retained device commands change or duplicate raw SDK stream order")
