"""Bind same-run service evidence and request boundaries to observed execution."""

from collections import Counter, defaultdict
from dataclasses import dataclass
from itertools import chain
import json

from markov_workflow.cost_samples import ObservedServices
from markov_workflow.latency import RequestLatency

from .fixed_context import QueueResumption
from .source_clock import ClockRealization


@dataclass(frozen=True, slots=True)
class ServiceInterval:
    rank: int
    index: int
    piece: dict
    service_ns: int | None
    basis: str
    resumptions: tuple[QueueResumption, ...]


class FixedReference:
    def __init__(self, case):
        self.case = case
        self.windows = {rank: tuple(contents.graph["evidence"]["window_ns"])
                        for rank, (contents, _, _) in case.sources.items()}
        self.peers = {}
        for source, target in case.connections.native_communication_edges:
            self.peers.setdefault(target, set()).add(source)
        self.gaps = {}
        self.requests = self.align_requests()
        self.specialization_loads = {rank: raw.get("specialization_loads") for rank, (_, raw, _) in case.sources.items()}

        # Timings are derived from one certified clock realization of the original case.
        self.clock = ClockRealization(case.clocks, case.graph).solve()
        self.entries = self.entry_releases()
        self.intervals = list(self.services())
        self.observer_joins = {(rank, event) for rank, record in case.measurements.items()
                               for event in record["observer_joins"]}
        fences = {(rank, source, target): duration for rank, record in case.measurements.items()
                  for (source, target), duration in record["host_fence_service"].items()}
        dispatches = {(rank, start): dispatch for rank, record in case.measurements.items()
                      for start, dispatch in record["device_releases"].items()}
        predecessors = self.predecessors(self.observer_joins | dispatches.keys() | {(rank, target) for rank, _, target in fences})
        self.event_windows = self.completion_windows(predecessors)
        self.outputs = self.publications()
        self.host_conditions = {(rank, source, target): conditions for rank, record in case.measurements.items()
                                for (source, target), conditions in record["fixed_host_delays"].items()}
        self.host_fences = self.bind_host_fences(fences, predecessors)
        self.device_conditions, self.device_dispatch = self.bind_device_dispatches(dispatches, predecessors)
        unbound = {rank: record["unbound_queue_notifications"] for rank, record in case.measurements.items()
                   if record["unbound_queue_notifications"]}
        if unbound:
            self.gaps["unbound_queue_notifications"] = unbound
        protocol = sum(event < 0 for _, event in case.graph.positions)
        if protocol:
            self.gaps["external_protocol_endpoints"] = protocol

    def predecessors(self, targets):
        parents = defaultdict(list)
        for source, following in self.case.graph.next.items():
            for target in following.keys() & targets:
                parents[target].append(source)
        return parents

    def bind_host_fences(self, fences, predecessors):
        positions = self.case.graph.positions
        qualified, unqualified = {}, []
        for (rank, begin, end), service in fences.items():
            entry = rank, begin
            parents = predecessors[rank, end]
            if service is None or entry not in parents:
                unqualified.append((rank, begin, end))
                continue

            prefix, tail = service
            sdk_entry = self.event_windows[positions[entry]][0] + prefix
            readiness = [self.event_windows[positions[parent]] for parent in parents if parent != entry]

            # Device readiness at the inner SDK entry qualifies the measured CPU-only service.
            if any(window is None or window[1] > sdk_entry for window in readiness):
                unqualified.append((rank, begin, end))
                continue
            qualified[rank, end] = entry, prefix, tail
        if unqualified:
            self.gaps["host_fence_service_unqualified"] = {"count": len(unqualified), "first": unqualified[0]}
        return qualified

    def bind_device_dispatches(self, dispatches, predecessors):
        positions = self.case.graph.positions
        delays, counts = {}, Counter()
        for (rank, start), dispatch in dispatches.items():
            target = rank, start
            source = rank, dispatch.submitted
            parents = predecessors[target]
            previous = rank, dispatch.previous
            preceding = dispatch.preceding_window_ns
            driver_entry = self.event_windows[positions[source]][0]
            if previous in parents and previous not in self.observer_joins:
                frontier = {source, previous}
                basis = "observed_preceding_task"
            elif preceding is not None and preceding[1] <= driver_entry:
                frontier = {source}
                basis = "preceding_task_already_complete"
            else:
                counts["stream_state_unqualified"] += 1
                continue

            release = max(self.event_windows[positions[event]][0] for event in frontier)
            others = [self.event_windows[positions[parent]] for parent in parents if parent not in frontier]
            if source not in parents or any(window is None or window[1] > release for window in others):
                counts["readiness_unqualified"] += 1
                continue

            # Both the driver post and the stream predecessor remain prerequisites when the frontier changes.
            ready = max(self.clock.timestamp(*event) for event in frontier)
            delays[target] = self.clock.timestamp(*target) - ready
            counts[basis] += 1
        return delays, {"candidates": len(dispatches), "attached": len(delays), **counts}

    def align_requests(self):
        case = self.case
        deliveries = []
        for index, instance in enumerate(case.manifest["instances"]):
            directory = case.capture / "instances" / str(index)
            delivery, = json.loads((directory / "request-delivery.json").read_text())["capture"]
            if delivery["clock"] != "monotonic_raw_ns":
                raise ValueError("fixed comparison requires the common RAW workload clock")
            deliveries.append(delivery)
            for rank in instance["native_participants"]:
                _, raw, _ = case.sources[rank]
                for row in raw["request_ingress"]:
                    if row["received_raw_ns"] > self.windows[rank][0]:
                        self.gaps.setdefault("ingress_inside_observation", {"rank": rank, **row})
        self.origin_ns, = {row["origin_ns"] for row in deliveries}

        intake, = [index for index, instance in enumerate(case.manifest["instances"]) if instance["mode"] != "decode"]
        requests = case.sessions[intake]["content"]["requests"]
        submissions = {index: row["submitted_ns"] for row in deliveries[intake]["submissions"] for index in row["requests"]}
        if submissions.keys() != set(range(len(requests))):
            raise ValueError("native submission records do not cover the workload")
        return [{**row, "arrival_ns": submissions[index]} for index, row in enumerate(requests)]

    def services(self):
        case = self.case
        for rank, (contents, raw, _) in case.sources.items():
            graph = contents.graph
            document = case.measurements[rank]
            measurements = document["measurements"]
            captured_collectives = {(lane, begin, end) for _, lane, begin, end, payload in measurements.get("collectives", ())
                                    if payload is not None}
            services = ObservedServices(graph, document, {})
            isolated = case.measured_service_pairs(rank, graph, raw, resources=document["resources"])
            for index, piece in enumerate(graph["work"]):
                start, finish = piece["start"], piece["finish"]
                host = piece["kind"] == "host"
                if host:
                    duration = services.host_service(piece)
                    basis = "observed_host_occupancy" if duration is not None else "missing"
                elif piece["kind"] == "collective":
                    duration = self.collective(rank, graph, piece, captured_collectives)
                    basis = "observed_collective" if duration is not None else "missing"
                else:
                    duration, _, _ = services.service(piece)
                    payload = document["device_payload_times"].get((start, finish))
                    if duration is not None and payload:
                        duration = sum(self.clock.device_duration(rank, first, last) for first, last in payload)
                    basis = "observed_payload" if duration is not None else "missing"
                if duration is None and not host and (start, finish) in isolated:
                    duration = self.clock.timestamp(rank, finish) - self.clock.timestamp(rank, start)
                    basis = "isolated_observed_interval"

                partition, resumptions = document["fixed_queue_resumptions"].get((start, finish), (duration, ()))
                if resumptions and (duration != partition or not duration):
                    self.gaps.setdefault("unqualified_queue_service_partitions", []).append((rank, start, finish))
                yield ServiceInterval(rank, index, piece, duration, basis, resumptions)

    def collective(self, rank, graph, piece, captured):
        start, finish = piece["start"], piece["finish"]
        events = graph["evidence"]["events"]
        if (piece["resource"], events[start]["order"], events[finish]["order"]) not in captured or (rank, finish) not in self.peers:
            return None

        # Each observed completion includes service after its own required peers are ready.
        readiness = [self.clock.timestamp(*event) for event in self.peers[rank, finish] | {(rank, start)}]
        completion = self.clock.timestamp(rank, finish)
        return None if completion is None or None in readiness else completion - max(readiness)

    def publications(self):
        outputs = {}
        expected = {row["id"]: row["output_tokens"] for row in self.requests}
        for rank, (contents, raw, _) in self.case.sources.items():
            if raw["environment"]["implementation"]["disaggregation_mode"] == "prefill":
                continue
            intervals = {row["name"]: row for row in raw["host_observation"]["intervals"]}
            for scope in raw["scopes"]:
                published = scope.get("publication")
                if not published:
                    continue
                node = contents.graph["scopes"][contents.source_ids[scope["id"]]]
                finish, = (p["event"] for p in node["ports"] if p["name"] == "call.return" and p["direction"] == "output")
                timestamp = intervals[f"markov.scope/{scope['id']}"]["end_raw_ns"]
                if self.case.clocks[rank].window(finish) != (timestamp, timestamp):
                    raise ValueError("publication does not match its observed scheduler return")
                for item in published:
                    key = item["id"], item["generated_tokens"]
                    event = (rank, finish)
                    if outputs.setdefault(key, event) != event:
                        raise ValueError("token has more than one native publication boundary")
        if outputs.keys() != {(request, token) for request, count in expected.items() for token in range(1, count + 1)}:
            self.gaps["incomplete_publication_boundaries"] = len(outputs)
        return outputs

    def detail(self, interval):
        piece = interval.piece
        graph = self.case.sources[interval.rank][0].graph
        scope = graph["scopes"][piece["scope"]]
        return {"rank": interval.rank, "work_index": interval.index, "kind": piece["kind"],
                "scope": scope["role"], "site": scope["site"], "start": piece["start"], "finish": piece["finish"]}

    def entry_releases(self):
        causal = self.case.graph
        has_predecessor = {target for targets in causal.next.values() for target in targets}
        entries = {}
        for rank, event in causal.positions:
            if event < 0 or (rank, event) in has_predecessor:
                continue
            graph = self.case.sources[rank][0].graph
            resource = graph["resources"][graph["events"][event]["resource"]]
            seen = self.case.clocks[rank].window(event)
            if resource["kind"] != "thread" or seen is None or seen[0] != seen[1] or seen[0] < self.origin_ns:
                self.gaps.setdefault("unqualified_entry_boundaries", []).append({"rank": rank, "event": event})
                continue

            # A measured host entry releases the fixed graph; internal boundaries follow simulated dependencies.
            entries[rank, event] = seen[0] - self.origin_ns
        return entries

    def timeline(self):
        graph = self.case.graph
        indices = graph.positions
        releases_ns = [self.entries.get(event, 0) for event in indices]
        fixed = {(indices[rank, source], indices[rank, target]): sum(conditions.values())
                 for (rank, source, target), conditions in self.host_conditions.items()}
        dependencies, work = [], []
        for interval in self.intervals:
            rank, piece = interval.rank, interval.piece
            start, finish = indices[rank, piece["start"]], indices[rank, piece["finish"]]
            delay = fixed.pop((start, finish), 0)
            if piece["kind"] == "collective":
                gate = len(releases_ns)
                releases_ns.append(0)
                readiness = self.peers[rank, piece["finish"]] | {(rank, piece["start"])}
                dependencies.extend((indices[predecessor], gate, 0) for predecessor in readiness)
                start = gate

            fixed_prefix = 0
            for resumption in interval.resumptions:
                resumed = len(releases_ns)
                releases_ns.append(0)
                producer = indices[rank, resumption.producer]

                # The consumer reaches its wait after its own progress and resumes after the queue handoff.
                work.append((start, resumed, resumption.fixed_prefix_ns - fixed_prefix))
                dependencies.append((producer, resumed, resumption.retirement_ns + resumption.dispatch_ns))
                start, fixed_prefix = resumed, resumption.fixed_prefix_ns
            work.append((start, finish, delay - fixed_prefix))

        # Fences and device dispatches add measured handling after their causal frontier.
        joins = {target: (None, 0, tail) for target, tail in self.device_conditions.items()}
        joins.update(self.host_fences)
        gates = {}
        for target, (entry, prefix, tail) in joins.items():
            gate = len(releases_ns)
            releases_ns.append(0)
            dependencies.append((gate, indices[target], tail))
            gates[target] = gate, entry, prefix

        # Fixed delays at regular dependencies also include measured handoffs between distinct host threads.
        for source, targets in graph.next.items():
            for target in targets:
                pair = indices[source], indices[target]
                delay = fixed.pop(pair, 0)
                if target in gates:
                    gate, entry, prefix = gates[target]
                    dependencies.append((pair[0], gate, delay + (prefix if source == entry else 0)))
                else:
                    dependencies.append((*pair, delay))
        if fixed:
            raise ValueError("fixed timing condition does not belong to a verified dependency")
        return {"releases_ns": releases_ns, "dependencies": dependencies, "work": work}

    def replay_services(self, durations):
        steps = []
        for interval, duration in zip(self.intervals, durations, strict=True):
            previous = 0
            for resumption in interval.resumptions:

                # Fixed scenarios preserve where CPU activity reached the wait along the observed work.
                prefix = duration * resumption.service_prefix_ns // interval.service_ns
                steps.append(prefix - previous)
                previous = prefix
            steps.append(duration - previous)
        return steps

    def completion_windows(self, predecessors):
        graph = self.case.graph
        windows = {}
        for rank, event in graph.positions:
            key = rank, event
            observed = self.case.clocks[rank].window(event) if event >= 0 else None
            if key not in self.observer_joins:
                windows[key] = observed
                continue

            # At a logical join the latest original prerequisite makes the result ready.
            sources = [windows[parent] for parent in predecessors[key]]
            if observed is None or not sources or None in sources:
                self.gaps.setdefault("unqualified_observer_joins", []).append(key)
                windows[key] = None
                continue
            first = max(begin for begin, _ in sources)
            last = min(observed[1], max(end for _, end in sources))
            if first > last:
                raise ValueError("observer marker precedes a verified join prerequisite")
            windows[key] = first, last
        return list(windows.values())

    def check_baseline(self, replayed):
        checked, differences = 0, Counter()
        first = None
        for ((rank, event), position), bounds in zip(self.case.graph.positions.items(), self.event_windows, strict=True):
            if bounds is None:
                continue
            checked += 1
            time = replayed[position]
            begin, end = (stamp - self.origin_ns for stamp in bounds)
            if begin <= time <= end:
                continue
            differences["early" if time < begin else "late"] += 1
            if first is None or begin < first["measured_window_ns"][0]:
                scope = self.case.sources[rank][0].graph["events"][event]["scope"]
                first = {"rank": rank, "event": event, "scope": scope, "measured_window_ns": [begin, end], "replayed_ns": time}
        return {"checked_boundaries": checked, "observer_joins": len(self.observer_joins),
                "differences": dict(differences), "first_difference": first}

    def local_unexplained_waits(self, plan, services):
        original = len(self.event_windows)
        latest = [None if window is None else window[1] - self.origin_ns for window in self.event_windows]
        latest.extend([0] * (len(plan["releases_ns"]) - original))
        for source, target, lag in plan["dependencies"]:
            if target >= original and latest[target] is not None:
                stamp = latest[source]
                latest[target] = None if stamp is None else max(latest[target], stamp + lag)

        incoming = plan["releases_ns"].copy()
        unknown = set()
        work = ((source, target, lag + service)
                for (source, target, lag), service in zip(plan["work"], services, strict=True))
        for source, target, lag in chain(plan["dependencies"], work):
            if target >= original:
                continue
            stamp = latest[source]
            if stamp is None:
                unknown.add(target)
            else:
                incoming[target] = max(incoming[target], stamp + lag)

        # Use original predecessor evidence here so earlier replay drift cannot create a local gap.
        checked, groups = 0, {}
        for (rank, event), position in self.case.graph.positions.items():
            window = self.event_windows[position]
            if window is None or position in unknown:
                continue
            checked += 1
            gap = window[0] - self.origin_ns - incoming[position]
            if gap <= 0:
                continue
            graph = self.case.sources[rank][0].graph
            resource = graph["resources"][graph["events"][event]["resource"]]
            role = "observer_join" if (rank, event) in self.observer_joins else resource["role"]
            group = groups.setdefault(role, {"count": 0, "max_lower_ns": 0})
            group["count"] += 1
            if gap > group["max_lower_ns"]:
                group.update(max_lower_ns=gap, rank=rank, event=event)
        return {"checked_boundaries": checked, "by_role": groups}

    def metrics(self, times):
        count = {row["id"]: row["output_tokens"] for row in self.requests}
        result = [{"request": request, "generated_tokens": token, "finished": token == count[request],
                   "time_ns": times[self.case.graph.positions[event]]} for (request, token), event in sorted(self.outputs.items())]
        return RequestLatency(result, self.requests).report()
