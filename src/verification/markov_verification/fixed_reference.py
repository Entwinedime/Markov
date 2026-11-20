"""Bind same-run service evidence and request boundaries to observed execution."""

from collections import Counter, defaultdict
from dataclasses import dataclass
from itertools import chain
import json

from markov_workflow.cost_samples import ObservedServices
from markov_workflow.latency import RequestLatency

from .fixed_context import ExternalWake, HostProgress
from .ingress import IngressEvidence
from .source_clock import ClockRealization, CounterPoint


@dataclass(frozen=True, slots=True)
class ServiceInterval:
    rank: int
    index: int
    piece: dict
    service_ns: int | None
    basis: str
    cuts: tuple[HostProgress, ...]


@dataclass(frozen=True, slots=True)
class FixedDeviceRecord:
    post: int | HostProgress
    duration_ns: int
    finished_ns: int


class FixedTimeline:
    def __init__(self, reference):
        self.reference = reference
        self.indices = reference.case.graph.positions
        self.releases = [reference.entries.get(event, 0) for event in self.indices]
        self.dependencies, self.work = [], []
        take_releases = {(rank, wait.take_entry): wait.take_entry.observed_ns - reference.origin_ns
                         for (rank, _), wait in reference.host_waits.items() if wait.take_entry is not None}
        self.progress = {(rank, cut): self.event(max(0, take_releases.get((rank, cut), 0)))
                         for (rank, _, _), cuts in reference.host_cuts.items() for cut in cuts}

    def event(self, release_ns=0):
        position = len(self.releases)
        self.releases.append(release_ns)
        return position

    def resume(self, rank, at, wait):
        resumed = self.event()
        self.dependencies.append((at, resumed, 0))
        waker = wait.source
        if isinstance(waker, ExternalWake):
            ready = waker.raw_ns - self.reference.origin_ns + wait.delay_ns
            self.releases[resumed] = max(self.releases[resumed], ready)
        else:
            source = self.progress[rank, waker] if isinstance(waker, HostProgress) else self.indices[rank, waker]
            self.dependencies.append((source, resumed, wait.delay_ns))
        return resumed

    def path(self, rank, start, finish, delay, cuts, *, modeled):
        edges = self.work if modeled else self.dependencies
        fixed_prefix = 0
        for cut in cuts:
            at = self.progress[rank, cut]
            edges.append((start, at, cut.fixed_prefix_ns - fixed_prefix))
            start, fixed_prefix = at, cut.fixed_prefix_ns
            if wait := self.reference.host_waits.get((rank, cut)):
                start = self.resume(rank, at, wait)
        edges.append((start, finish, delay - fixed_prefix))

    def document(self):
        return {"releases_ns": self.releases, "dependencies": self.dependencies, "work": self.work}


class FixedReference:
    def __init__(self, case):
        self.case = case
        self.peers = {}
        for source, target in case.connections.native_communication_edges:
            self.peers.setdefault(target, set()).add(source)
        self.gaps = {}
        self.input_releases = {}
        self.requests = self.align_requests()
        self.specialization_loads = {rank: raw.get("specialization_loads") for rank, (_, raw, _) in case.sources.items()}

        dispatches = {(rank, start): dispatch for rank, record in case.measurements.items()
                      for start, dispatch in record["device_releases"].items()}
        self.clock = ClockRealization(case.clocks, case.graph)
        for target, dispatch in dispatches.items():
            submitted = target[0], dispatch.submitted

            # The verified driver submission also precedes a measured wait's entry.
            if dispatch.stream_wait and target not in case.graph.next[submitted]:
                self.clock.constrain_order(submitted, target)
        self.clock.solve()
        self.host_waits = self.qualified_host_waits()
        self.observer_joins = {(rank, event) for rank, record in case.measurements.items()
                               for event in record["observer_joins"]}
        self.elided_waits = {(rank, event) for rank, events in case.logical.items() for event in events}
        fences = [(rank, source, target) for rank, record in case.measurements.items()
                  for source, target in sorted(record["host_fences"])]
        stream_waits = [(rank, start, row.wait_finish) for (rank, start), row in dispatches.items()
                        if row.wait_finish is not None and (rank, row.wait_finish) not in self.observer_joins]
        completions = {(rank, target) for rank, _, target in chain(fences, stream_waits)}
        joins = self.observer_joins | self.elided_waits
        predecessors = self.predecessors(joins | dispatches.keys() | completions)
        self.event_windows = self.completion_windows(predecessors)
        self.outputs = self.publications()
        self.host_conditions = {((rank, source), (rank, target)): conditions for rank, record in case.measurements.items()
                                for (source, target), conditions in record["fixed_host_delays"].items()}
        self.host_fences, self.fence_evidence = self.bind_completions(
            fences, predecessors, "host_fence_timing_unqualified"
        )
        self.device_records = {}
        self.device_conditions, self.wait_submissions, self.device_dispatch = self.bind_device_dispatches(
            dispatches, predecessors, joins
        )
        wait_conditions, self.device_dispatch["wait_completions"] = self.bind_completions(
            stream_waits, predecessors, "stream_wait_timing_unqualified"
        )
        self.device_conditions.update(wait_conditions)
        self.host_cuts = self.host_progress_cuts()
        self.intervals = list(self.services())
        self.entries = self.entry_releases()
        unbound = {rank: record["unbound_queue_notifications"] for rank, record in case.measurements.items()
                   if record["unbound_queue_notifications"]}
        if unbound:
            self.gaps["unbound_queue_notifications"] = unbound
        protocol = sum(event < 0 for _, event in case.graph.positions)
        if protocol:
            self.gaps["external_protocol_endpoints"] = protocol

    def qualified_host_waits(self):
        owners = {thread: rank for rank, record in self.case.measurements.items() for thread in record["kernel_threads"]}
        waits = {}
        for rank, record in self.case.measurements.items():
            for wait in record["fixed_host_waits"]:
                if isinstance(wait.source, ExternalWake) and wait.source.kernel_thread in owners:

                    # A wake from another projected worker needs a causal edge, never an absolute release.
                    owner = owners[wait.source.kernel_thread]
                    self.gaps.setdefault("unbound_worker_wakes", []).append([rank, wait.stopped.start, wait.stopped.finish, owner])
                    continue
                waits[rank, wait.stopped] = wait
        return waits

    def host_progress_cuts(self):
        cuts = defaultdict(set)
        for (rank, stopped), wait in self.host_waits.items():
            for cut in (stopped, wait.source, wait.take_entry):
                if isinstance(cut, HostProgress):
                    cuts[rank, cut.start, cut.finish].add(cut)
        for (rank, _), (_, records) in self.device_records.items():
            for record in records:
                post = record.post
                if isinstance(post, HostProgress):
                    cuts[rank, post.start, post.finish].add(post)
        return {step: tuple(sorted(points, key=lambda point: point.observed_ns)) for step, points in cuts.items()}

    def predecessors(self, targets):
        parents = defaultdict(list)
        for source, following in self.case.graph.next.items():
            for target in following.keys() & targets:
                parents[target].append(source)
        return parents

    def bind_completions(self, waits, predecessors, gap):
        positions = self.case.graph.positions
        qualified, unqualified = {}, []
        counts = Counter()
        for rank, begin, end in waits:
            entry, returned = (rank, begin), (rank, end)
            parents = self.readiness_parents(returned, predecessors, self.elided_waits)
            if entry not in parents:
                unqualified.append((rank, begin, end))
                continue

            entered = self.clock.timestamp(*entry)
            finish = self.clock.timestamp(*returned)
            readiness = {parent: self.event_windows[positions[parent]] for parent in parents}
            if any(window is None or parent in self.observer_joins and window[1] > entered
                   for parent, window in readiness.items()):
                unqualified.append((rank, begin, end))
                continue

            # A verified completion follows the actual producer and the wait entry on either lane.
            ready = max(self.clock.timestamp(*parent) + sum(self.host_conditions.get((parent, returned), {}).values())
                        for parent in parents if parent not in self.observer_joins)
            if ready > finish:
                unqualified.append((rank, begin, end))
                continue

            qualified[returned] = finish - ready
            counts["frontier_in_call" if ready > entered else "ready_on_entry"] += 1
            counts["return_delay_ns"] += finish - ready
        if unqualified:
            self.gaps[gap] = {"count": len(unqualified), "first": unqualified[0]}
        return qualified, {"qualified": len(qualified), **counts}

    @staticmethod
    def readiness_parents(target, predecessors, joins):
        pending = list(predecessors[target])
        seen, physical = set(), set()

        # Follow the actual prerequisites of untimed joins while retaining their edges in the replay.
        for parent in pending:
            if parent in seen:
                continue
            seen.add(parent)
            if parent in joins:
                pending.extend(predecessors[parent])
            else:
                physical.add(parent)
        return physical

    def bind_omitted_records(self, rank, dispatch, parents):
        previous = rank, dispatch.previous
        if not dispatch.omitted or previous not in parents:
            return ()
        clock = self.case.clocks[rank]
        posts = self.case.measurements[rank]["fixed_driver_posts"]
        previous_finish = self.clock.timestamp(*previous)
        records = []
        for omitted in dispatch.omitted:
            host = posts.get(omitted)
            point = clock.point(omitted.completed_device_ns, True)
            earliest, _ = clock.point_window(point)
            finish = self.clock.raw_time(rank, point)
            ready = max(omitted.posted_ns, previous_finish)
            if host is None or earliest < omitted.posted_ns or ready > finish:
                return ()
            records.append(FixedDeviceRecord(host, finish - ready, finish))
            previous_finish = finish
        return tuple(records)

    def frontier_ready(self, rank, frontier, parents, completed=None):
        positions = self.case.graph.positions
        windows = [self.event_windows[positions[event]] for event in frontier]
        if None in windows:
            return False
        earliest = max(window[0] for window in windows)
        clock = self.case.clocks[rank]
        points = [clock.points[event] for _, event in frontier]
        if completed is not None:
            point = clock.point(completed, True)
            points.append(point)
            earliest = max(earliest, clock.point_window(point)[0])
        device = max((point.nominal_ns for point in points if isinstance(point, CounterPoint)), default=None)

        for parent in parents:
            if parent in frontier:
                continue
            window = self.event_windows[positions[parent]]
            if window is not None and window[1] <= earliest:
                continue

            # Ordering on a shared device counter survives overlapping RAW calibration windows.
            actor, event = parent
            point = clock.points[event] if actor == rank and event >= 0 else None
            if device is None or not isinstance(point, CounterPoint) or point.nominal_ns > device:
                return False
        return True

    def bind_device_dispatches(self, dispatches, predecessors, joins):
        positions = self.case.graph.positions
        delays, wait_submissions, counts = {}, {}, Counter()
        for (rank, start), dispatch in dispatches.items():
            target = rank, start
            if target in self.observer_joins:
                continue
            source = rank, dispatch.submitted
            parents = self.readiness_parents(target, predecessors, joins)
            previous = rank, dispatch.previous
            preceding = dispatch.preceding_window_ns
            posted = self.event_windows[positions[source]]
            observed_start = self.event_windows[positions[target]]
            if posted is None or observed_start is None:
                counts["readiness_unqualified"] += 1
                continue

            driver_entry = posted[0]
            records = ()
            if not dispatch.omitted and previous in parents:
                frontier = {source, previous}
                basis = "observed_preceding_task"
            elif dispatch.first_command:
                frontier = {source}
                basis = "first_observed_command"
            elif preceding is not None and preceding[1] <= driver_entry:
                frontier = {source}
                basis = "preceding_task_already_complete"
            elif records := self.bind_omitted_records(rank, dispatch, parents):
                frontier = {source, previous}
                basis = "observed_unpublished_record"
            else:
                counts["stream_state_unqualified"] += 1
                continue

            completed = dispatch.omitted[-1].completed_device_ns if records else None
            if not self.frontier_ready(rank, frontier, parents, completed):
                counts["readiness_unqualified"] += 1
                continue

            # The coarse wait records its submission at completion. Replay also retains the measured SDK entry.
            missing_submission = source not in parents
            if missing_submission and not dispatch.stream_wait:
                counts["readiness_unqualified"] += 1
                continue

            # Both the driver post and the stream predecessor remain prerequisites when the frontier changes.
            ready = max(self.clock.timestamp(*event) for event in frontier)
            if records:
                ready = max(ready, records[-1].finished_ns)
            duration = self.clock.timestamp(*target) - ready
            if duration < 0:
                counts["readiness_unqualified"] += 1
                continue

            if missing_submission:
                wait_submissions[target] = source
                counts["verified_wait_submissions"] += 1

            delays[target] = duration
            if records:
                self.device_records[target] = (dispatch.previous, records)
            counts[basis] += 1
        return delays, wait_submissions, {"candidates": len(dispatches), "attached": len(delays), **counts}

    def align_requests(self):
        case = self.case
        deliveries = []
        instances = case.manifest["instances"]
        for index, _ in enumerate(instances):
            directory = case.capture / "instances" / str(index)
            delivery, = json.loads((directory / "request-delivery.json").read_text())["capture"]
            if delivery["clock"] != "monotonic_raw_ns":
                raise ValueError("fixed comparison requires the common RAW workload clock")
            deliveries.append(delivery)
        self.origin_ns, = {row["origin_ns"] for row in deliveries}

        intake = []
        for instance, session, delivery in zip(instances, case.sessions, deliveries, strict=True):
            requests = session["content"]["requests"]
            submissions = [(index, row["submitted_ns"]) for row in delivery["submissions"] for index in row["requests"]]
            if sorted(index for index, _ in submissions) != list(range(len(requests))):
                raise ValueError("native submission records must cover every request exactly once")
            submitted = dict(submissions)
            if instance["mode"] != "decode":
                intake.append([{**row, "arrival_ns": submitted[index]} for index, row in enumerate(requests)])
            self.bind_ingress(instance, session, submitted)
        workload, = intake
        return workload

    def bind_ingress(self, instance, session, submitted):
        requests = {request["id"]: request for request in session["content"]["requests"]}
        aliases = dict(zip(session["source_requests"], requests, strict=True))
        send_ns = {identity: self.origin_ns + submitted[index] for index, identity in enumerate(aliases)}
        for rank in instance["native_participants"]:
            contents, raw, _ = self.case.sources[rank]
            if not raw["request_ingress"]:
                continue
            IngressEvidence.rounds(raw["request_ingress"], aliases, requests, self.origin_ns)
            start, _ = contents.graph["evidence"]["window_ns"]
            wakes = self.case.measurements[rank]["fixed_idle_inputs"]
            for record in raw["request_ingress"]:
                received = record["received_raw_ns"]
                stamps = [send_ns[request] for request in record["requests"]]
                if any(stamp > received for stamp in stamps):
                    raise ValueError("request receipt precedes its actual submission")
                if received <= start:
                    continue
                cut = wakes.get(received)

                # Use the idle receiver's actual CPU release only for requests submitted during that wait.
                if cut and all(cut.stopped_raw_ns <= stamp <= cut.resumed_raw_ns for stamp in stamps):
                    self.input_releases[(rank, cut.source), (rank, cut.target)] = cut
                else:
                    self.gaps.setdefault("unqualified_input_readiness", []).append({"rank": rank, **record})

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

                cuts = self.host_cuts.get((rank, start, finish), ())
                if any(cut.service_ns != duration for cut in cuts):
                    self.gaps.setdefault("unqualified_host_service_partitions", []).append((rank, start, finish))
                yield ServiceInterval(rank, index, piece, duration, basis, cuts)

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
            if event < 0 or (rank, event) in has_predecessor or (rank, event) in self.wait_submissions:
                continue
            if (rank, event) in self.elided_waits:
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
        timeline = FixedTimeline(self)
        indices = timeline.indices
        fixed = {(indices[source], indices[target]): sum(conditions.values())
                 for (source, target), conditions in self.host_conditions.items()}
        order_cuts = self.host_cuts.copy()

        for interval in self.intervals:
            rank, piece = interval.rank, interval.piece
            begin, end = piece["start"], piece["finish"]
            start, finish = indices[rank, begin], indices[rank, end]
            delay = fixed.pop((start, finish), 0)
            order_cuts.pop((rank, begin, end), None)
            if piece["kind"] == "collective":
                start = timeline.event()
                readiness = self.peers[rank, end] | {(rank, begin)}
                timeline.dependencies.extend((indices[predecessor], start, 0) for predecessor in readiness)
            timeline.path(rank, start, finish, delay, interval.cuts, modeled=True)

        # Fences and device dispatches add measured handling after their causal frontier.
        gates = {}
        for target, tail in (self.device_conditions | self.host_fences).items():
            gate = timeline.event()
            timeline.dependencies.append((gate, indices[target], tail))
            gates[target] = gate
        for target, source in self.wait_submissions.items():
            timeline.dependencies.append((indices[source], gates[target], 0))

        # Unpublished records use their measured submission and stream predecessor on the fixed timeline.
        for target, (previous, records) in self.device_records.items():
            rank, _ = target
            predecessor = indices[rank, previous]
            for record in records:
                post = timeline.progress[rank, record.post] if isinstance(record.post, HostProgress) else indices[rank, record.post]
                complete = timeline.event()
                timeline.dependencies.extend(((post, complete, record.duration_ns),
                                              (predecessor, complete, record.duration_ns)))
                predecessor = complete
            timeline.dependencies.append((predecessor, gates[target], 0))

        # Fixed delays at regular dependencies also include measured handoffs between distinct host threads.
        for source, targets in graph.next.items():
            for target in targets:
                pair = indices[source], indices[target]
                delay = fixed.pop(pair, 0)
                origin = pair[0]
                if observed := self.input_releases.get((source, target)):
                    gate = timeline.event(observed.resumed_raw_ns - self.origin_ns)
                    timeline.dependencies.append((origin, gate, observed.head_ns))
                    origin, delay = gate, observed.tail_ns
                cuts = order_cuts.pop((source[0], source[1], target[1]), ()) if source[0] == target[0] else ()
                timeline.path(source[0], origin, gates.get(target, pair[1]), delay, cuts, modeled=False)
        if fixed or order_cuts:
            raise ValueError("fixed timing condition does not belong to a verified dependency")
        return timeline.document()

    def replay_services(self, durations):
        steps = []
        for interval, duration in zip(self.intervals, durations, strict=True):
            previous = 0
            for cut in interval.cuts:

                # Fixed scenarios preserve the measured progress at each intra-work boundary.
                prefix = duration * cut.service_prefix_ns // interval.service_ns
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
            parents = self.readiness_parents(key, predecessors, self.elided_waits)
            sources = [windows[parent] for parent in parents]
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
        latest.extend(plan["releases_ns"][original:])
        work = ((source, target, lag + service)
                for (source, target, lag), service in zip(plan["work"], services, strict=True))
        incoming = plan["releases_ns"].copy()
        unknown = set()
        outgoing, remaining = defaultdict(list), Counter()
        for source, target, lag in chain(plan["dependencies"], work):
            outgoing[source].append((target, lag))
            if target >= original:
                remaining[target] += 1

        # Original boundaries are fixed witnesses here. Only auxiliary cuts need causal propagation.
        ready = list(range(original))
        ready.extend(index for index in range(original, len(latest)) if not remaining[index])
        for source in ready:
            stamp = latest[source]
            for target, lag in outgoing[source]:
                if target < original:
                    if stamp is None:
                        unknown.add(target)
                    else:
                        incoming[target] = max(incoming[target], stamp + lag)
                    continue
                if latest[target] is not None:
                    latest[target] = None if stamp is None else max(latest[target], stamp + lag)
                remaining[target] -= 1
                if not remaining[target]:
                    ready.append(target)

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
