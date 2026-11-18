"""Prepare native execution conditions and independently check their actual consumption."""

import argparse
import json
from collections import Counter, defaultdict
from pathlib import Path

from markov_adapters.artifacts import write_json
from markov_adapters.pytorch import CpuTransfer

from actions import ActionIdentity
from context_values import ContextValues
from dependencies import ActionBoundaries
from ingress import IngressEvidence
from native_case import NativeCase
from source_clock import ClockRealization


class NativeInputs:
    def __init__(self, capture, graphs, output, layouts=None):
        self.capture, self.graphs, self.output = capture, graphs, output
        self.layouts = layouts
        self.steps = defaultdict(list)
        self.cpu_posts = defaultdict(dict)
        self.origins, self.ingress = {}, {}
        self.counts = Counter()
        self.contexts = ContextValues()
        self.layout_values = ContextValues()

    @staticmethod
    def read(path):
        return json.loads(path.read_text())

    @staticmethod
    def window(begin, end, origin):
        if begin > end or end < origin:
            raise ValueError("native condition has a reversed or pre-workload interval")
        return {"begin_ns": max(begin - origin, 0), "end_ns": end - origin}

    @staticmethod
    def port(name, direction, window):
        return {"at": {"port": name, "direction": direction}, "window": window}

    def actions(self, case, rank, realization, logical_times, origin):
        group, clock = case.groups[rank], case.clocks[rank]
        source, raw, identities = case.sources[rank]
        graph, evidence = source.graph, source.graph["evidence"]
        if evidence["clock"] != "monotonic_raw_ns":
            raise ValueError("native input conditions require the common RAW clock")
        layouts = None if self.layouts is None else self.read(self.layouts / f"{rank}-layouts.json")
        if layouts is not None and set(layouts) != {str(index) for index in identities}:
            raise ValueError("timing layouts and independently observed action scopes differ")
        if layouts is not None and sum(len(layout["identity"][-1]) for layout in layouts.values()) != len(graph["work"]):
            raise ValueError(f"rank {rank} action cuts omit or repeat native work")
        services = case.measured_service_pairs(rank, graph, raw) if layouts is not None else set()
        self.counts["unmeasured_work_finishes"] += sum(evidence["events"][work["finish"]]["time_ns"] is None for work in graph["work"])
        for index, identity in identities.items():
            scope = graph["scopes"][index]
            entered = next(port["event"] for port in scope["ports"] if port["name"] == "call.enter")
            begin = evidence["events"][entered]["time_ns"]
            if begin is None:
                raise ValueError(f"rank {rank} action {index} event {entered} has no measured entry")
            if "work" not in identity:
                raise ValueError(f"rank {rank} action {index} has no complete logical identity")
            record = {"operation": "action", "identity": self.contexts.encode({
                name: identity[name] for name in ("action", "parameters", "work")
            }), "source": {"scope": index}}
            if layouts is not None:
                layout = layouts[str(index)]
                record["layout"] = self.layout_values.intern(layout["identity"])
                record["services"] = [[start, finish] for _, _, start, finish in layout["identity"][-1]
                                      if (layout["events"][start], layout["events"][finish]) in services]
                record["timestamps"] = []
                for event in layout["events"]:
                    measured = clock.window(event)
                    if measured is None:
                        selected = logical_times.get((rank, event))
                        if selected is None:
                            raise ValueError(f"rank {rank} action {index} event {event} has no measured endpoint or proven logical time")
                        self.counts["derived_logical_boundaries"] += 1
                    else:
                        selected = realization.timestamp(rank, event)
                        self.counts["measured_boundaries"] += 1
                        self.counts["calibrated_boundary_ranges"] += measured[0] != measured[1]
                    time_ns = self.window(selected, selected, origin)["begin_ns"]
                    record["timestamps"].append([event, time_ns])
                self.counts["measured_work"] += len(layout["identity"][-1])
                self.counts["isolated_observed_services"] += len(record["services"])
                self.steps[group].append((begin, record, None))
                continue

            boundaries = []
            for boundary in case.completion_boundaries(rank, index):
                measured = clock.window(boundary["event"])
                if measured is None:
                    self.counts["unmeasured_boundaries"] += 1
                    continue
                selected = realization.timestamp(rank, boundary["event"])
                condition = self.window(selected, selected, origin)
                self.counts["measured_boundaries"] += 1
                self.counts["calibrated_boundary_ranges"] += measured[0] != measured[1]
                boundaries.append({"at": boundary["at"], "window": condition})
            bindings = source.invocation_bindings(index, group)
            self.counts["bound_invocations"] += bool(bindings)
            record.update(bindings=bindings, service_ns=None, boundaries=boundaries)
            self.steps[group].append((begin, record, None))

    def input_reads(self, instance, rank, raw, aliases, requests, origin):
        if not raw["request_ingress"]:
            return
        name, group = instance["id"], f"{instance['id']}/{rank}"
        if name in self.ingress:
            raise ValueError("native input has more than one external ingress per instance")
        self.ingress[name] = IngressEvidence.rounds(raw["request_ingress"], aliases, requests, origin)
        if IngressEvidence.attempts(raw)["status"] != "observed":
            raise ValueError("native input reads lack their complete entering contexts")
        reads = IngressEvidence.reads(name, raw, aliases, origin)
        for number, (interval, read) in enumerate(zip(raw["request_input_attempts"], reads, strict=True)):
            self.steps[group].append((interval["begin_raw_ns"], {
                "operation": "input", "identity": self.contexts.encode(read["identity"]), "source": {"input": number},
                "window": {"begin_ns": read["time_ns"], "end_ns": read["time_ns"]},
            }, None))

    def cpu(self, instance, rank, origin):
        group = f"{instance['id']}/{rank}"
        document = self.read(self.capture / "protocol" / f"rank-{rank}.json")
        for post in document["posts"]:
            key = tuple(post["key"])
            if rank in self.cpu_posts[key]:
                raise ValueError("native CPU input repeats a local post")
            self.cpu_posts[key][rank] = {"post": post, "group": group, "origin": origin, "position": None}
            intervals = [(post["interval"], (key, rank))]
            if post["operation"] in {"isend", "irecv"} and post["consumed"] is not None:
                intervals.append((post["consumed"], None))
            for interval, position in intervals:
                execution = interval["execution"]
                if not execution["in_flow"]:
                    continue
                work = execution["work"]
                context = work["context"]
                work = {**work, "context": {**context, "cpu_transfer": CpuTransfer.identity(context["cpu_transfer"])}}
                begin, end = interval["begin_raw_ns"], interval["end_raw_ns"]
                self.steps[group].append((begin, {
                    "operation": "cpu", "identity": self.contexts.encode({
                        "operation": execution["operation"], "work": ActionIdentity.work_identity(work),
                    }), "source": {"post": list(key), "phase": "post" if position is not None else "wait"}, "service_ns": None, "boundaries": [
                        self.port("call.enter", "input", self.window(begin, begin, origin)),
                        self.port("call.return", "output", self.window(end, end, origin)),
                    ],
                }, position))

    def transfers(self):
        result = []
        for peers in self.cpu_posts.values():
            if len(peers) != 2:
                continue
            consumed = [peer["post"]["consumed"]["end_raw_ns"] for peer in peers.values() if peer["post"]["consumed"] is not None]
            positions = sorted((peer["position"] for peer in peers.values() if peer["position"] is not None), key=lambda row: (row["group"], row["step"]))
            if not consumed or not positions:
                continue
            origins = {peer["origin"] for peer in peers.values()}
            if len(origins) != 1:
                raise ValueError("native CPU message crosses unrelated workload clocks")
            origin, = origins
            begin = max(peer["post"]["interval"]["begin_raw_ns"] for peer in peers.values())
            result.append({"posts": positions, "service_ns": None, "window": self.window(begin, min(consumed), origin)})
        return result

    def run(self):
        self.output.mkdir(parents=True)
        manifest = self.read(self.capture / "capture.json")
        evidence = NativeCase(self.capture, manifest, self.graphs)
        report = {"capture": str(self.capture.resolve()), "graphs": str(self.graphs.resolve()), "status": "unverified"}
        try:
            for number, instance in enumerate(manifest["instances"]):
                directory = self.capture / "instances" / str(number)
                session = self.read(directory / "session.json")
                delivery, = self.read(directory / "request-delivery.json")["capture"]
                if delivery["clock"] != "monotonic_raw_ns" or set(instance["observed_roles"]) != set(session["native_participants"]):
                    raise ValueError("native input needs the common clock and every actual participant")
                origin = delivery["origin_ns"]
                requests = {request["id"]: request for request in session["content"]["requests"]}
                aliases = dict(zip(session["source_requests"], requests, strict=True))
                for rank in session["native_participants"]:
                    group = f"{instance['id']}/{rank}"
                    self.origins[group] = origin
                    raw = self.read(self.capture / f"rank-{rank}.json")
                    graph = self.read(self.graphs / f"{rank}-graph.json")
                    evidence.observe(rank, graph, raw)
                    self.input_reads(instance, rank, raw, aliases, requests, origin)
                    self.cpu(instance, rank, origin)
                if instance["id"] not in self.ingress:
                    raise ValueError("native case omits external ingress")
            if len(set(self.origins.values())) != 1:
                raise ValueError("native serving instances have different workload origins")
            evidence.finish()
            native_report = evidence.report(False)
            report["native_evidence"] = native_report
            if evidence.graph is None or native_report["status"] == "differences":
                raise ValueError("native source has no consistent causal graph")
            realization = ClockRealization(evidence.clocks, evidence.graph).solve()
            logical_times = evidence.logical_times(realization) if self.layouts is not None else {}
            for rank in evidence.sources:
                group = evidence.groups[rank]
                self.actions(evidence, rank, realization, logical_times, self.origins[group])
            actors = {}
            for group in self.origins:
                ordered = sorted(self.steps[group], key=lambda step: step[0])
                if len({begin for begin, _, _ in ordered}) != len(ordered):
                    raise ValueError(f"native {group} has unordered operation entries at the same RAW timestamp")
                actors[group] = {"steps": [step for _, step, _ in ordered]}
                for position, (_, _, post) in enumerate(ordered):
                    if post is not None:
                        key, rank = post
                        self.cpu_posts[key][rank]["position"] = {"group": group, "step": position}
            notifications = [
                {"identity": {"group": row["group"], "name": name, "work": {"request": row["request"]}},
                 "window": self.window(row[field]["begin_raw_ns"], row[field]["end_raw_ns"], self.origins[row["group"]])}
                for row in evidence.readiness["endpoints"]
                for name, field in (("kv_ready", "reservation_update"), ("kv_success", "completion_update"))
                if row[field] is not None and (name != "kv_ready" or row["remote_reservation"])
            ]
            document = {"context_values": self.contexts.values, "actors": actors, "ingress": self.ingress,
                        "notifications": notifications, "transfers": self.transfers()}
            if self.layouts is not None:
                if self.cpu_posts:
                    raise ValueError("full timing cannot qualify undivided CPU protocol intervals")
                document["layouts"] = self.layout_values.values
            report.update(status="partial" if self.counts["unmeasured_boundaries"] or self.counts["unmeasured_work_finishes"]
                          or native_report["status"] != "consistent" else "extracted",
                          clock_realization=realization.report(), counts=dict(self.counts),
                          steps={group: len(actor["steps"]) for group, actor in actors.items()})
            if report["status"] == "extracted":
                write_json(self.output / "execution-inputs.json", document, replace=False)
        except (OSError, ValueError, KeyError, IndexError, TypeError) as error:
            report.update(status="unverified", error=str(error))
        report["scope"] = "Checked actor operations and one jointly feasible device-clock realization, rounded upward to integer nanoseconds. Original calibration uncertainty remains. Full timing requires an actual measurement or independently verified elided wait with known prerequisites. CPU wire and notification intervals remain explicit bounds."
        (self.output / "execution-inputs-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        return 0 if report["status"] == "extracted" else 1


class ConsumedInputs:
    """Prove consumed prefixes from native observations, independently of the supplied condition file."""

    def __init__(self, case, ingress):
        self.capture, self.groups, self.ingress = case.capture, case.groups, ingress
        self.sources = dict(case.sources)
        self.case = case
        self.logical_times = set()
        self.service_pairs, self.service_durations = {}, set()
        self.ranks = {group: rank for rank, group in case.groups.items()}
        self.origin_ns = ingress.origin_ns
        self.ports, self.cpu_times = {}, {}
        self.bindings = {}
        self.boundaries = defaultdict(set)
        self.realization = None
        self.expected = None
        self.positions = Counter()
        self.pending, self.reads = {}, {}
        self.input_pairs = defaultdict(dict)
        self.counts = Counter()
        self.measured_graph = None

    def prepare(self):
        ordered = defaultdict(list)
        for rank, (source, raw, identities) in self.sources.items():
            group = self.groups[rank]

            # Original CPU entries establish order; graph scope references retain their independently checked raw origins.
            host = {int(row["name"].split("/")[1]): row["begin_raw_ns"]
                    for row in raw["host_observation"]["intervals"] if row["name"].startswith("markov.scope/")}
            host.update({query["scope"]: query["begin_raw_ns"] for query in raw["state_queries"]})
            for index, identity in identities.items():
                self.ports[group, index] = [boundary for boundary in source.completion_boundaries(index)
                                            if boundary["event"] not in self.case.logical[rank]]
                if bindings := source.invocation_bindings(index, group):
                    self.bindings[group, index] = bindings
                origins = source.graph["scopes"][index]["bindings"]["source_scopes"]
                begin = min(raw["scopes"][origin]["host_interval"]["begin_raw_ns"] if "service" in raw["scopes"][origin] else host[origin]
                            for origin in origins)
                ordered[group].append((begin, {"operation": "action", "identity": {key: identity[key] for key in ("action", "parameters", "work")},
                                              "source": {"scope": index}}))

            protocol = json.loads((self.capture / f"protocol/rank-{rank}.json").read_text())
            for post in protocol["posts"]:
                for phase, interval in (("post", post["interval"]), ("wait", post["consumed"])):
                    if interval is None or not interval["execution"]["in_flow"] or (phase == "wait" and post["operation"] not in {"isend", "irecv"}):
                        continue
                    execution = interval["execution"]
                    self.cpu_times[group, tuple(post["key"]), phase] = interval["begin_raw_ns"], interval["end_raw_ns"]
                    work = execution["work"]
                    work = {**work, "context": {**work["context"], "cpu_transfer": CpuTransfer.identity(work["context"]["cpu_transfer"])}}
                    ordered[group].append((interval["begin_raw_ns"], {
                        "operation": "cpu", "identity": {"operation": execution["operation"], "work": ActionIdentity.work_identity(work)},
                        "source": {"post": post["key"], "phase": phase},
                    }))

            if raw["request_ingress"]:
                instance = group.rsplit("/", 1)[0]
                for index, (interval, read) in enumerate(zip(raw["request_input_attempts"], self.ingress.native_reads[instance], strict=True)):
                    ordered[group].append((interval["begin_raw_ns"], {
                        "operation": "input", "identity": read["identity"], "source": {"input": index},
                    }))

        self.expected = {}
        for group, records in ordered.items():
            records.sort(key=lambda item: item[0])
            if any(left[0] >= right[0] for left, right in zip(records, records[1:])):
                raise ValueError("native condition witnesses need strictly ordered measured CPU entries")
            self.expected[group] = [record for _, record in records]
    def observe(self, row):
        if self.measured_graph is not None:
            rank, root, target, event_ids, matches, durations = self.measured_graph
            if row["kind"] == "boundary_time" and "source_event" in row:
                target_event, source_event = row["event"], row["source_event"]
                if target_event in matches:
                    raise ValueError("full timing repeats a target event")
                matches[target_event] = source_event
                self.boundaries[target_event].add((rank, source_event))
            elif row["kind"] != "boundary_time":
                if matches:
                    self.check_measured_graph(rank, root, target, event_ids, matches, durations)
                self.measured_graph = None

        if row["kind"] == "condition_used":
            if self.expected is None:
                self.prepare()
                self.realization = ClockRealization(self.case.clocks, self.case.graph)
            group = row["group"]
            position = self.positions[group]
            if (row.keys() != {"kind", "group", "step", "operation", "source"} or type(row["step"]) is not int
                    or row["step"] != position or position >= len(self.expected[group]) or group in self.pending):
                raise ValueError(f"invalid consumed execution condition position: {group} at {position}")

            # The following graph or input read supplies the actual identity; the receipt only selects its original condition.
            record = self.expected[group][position]
            if row["operation"] != record["operation"] or row["source"] != record["source"]:
                raise ValueError(f"consumed execution condition differs from its native causal prefix: {group} at {position}")
            self.positions[group] += 1
            self.counts[record["operation"]] += 1
            if record["operation"] == "input":
                instance = record["identity"]["instance"]
                if instance in self.reads:
                    raise ValueError("input condition was consumed before the preceding read returned")
                self.reads[instance] = record["source"]["input"], record["identity"]
            else:
                self.pending[group] = record
        elif row["kind"] == "input_read" and self.expected is not None:
            instance = row["identity"]["instance"]
            if instance in self.reads:
                source, identity = self.reads.pop(instance)
            elif not row["in_flow"]:
                return
            else:
                raise ValueError("active input read has no consumed condition")
            if identity != row["identity"]:
                raise ValueError("input result belongs to a different consumed condition")
            self.input_pairs[instance][len(self.ingress.actual_reads[instance])] = source

        elif row["kind"] == "completion" and self.expected is not None:
            raw_ns = row["time_ns"] + self.origin_ns
            for rank, event in self.boundaries.pop(row["event"], ()):
                if rank is None:
                    if raw_ns != event:
                        raise ValueError("CPU protocol completion changes its original RAW boundary")
                elif event in self.case.logical[rank]:
                    self.logical_times.add((rank, event, raw_ns))
                else:
                    self.realization.condition(rank, event, raw_ns)
                self.counts["timed_boundaries"] += 1

    def graph(self, group, row, contents):
        if self.expected is None:
            return None
        root = row["graph"]["scopes"][0]
        record = self.pending.pop(group)
        if root["role"] == "cpu_service":
            execution = root["bindings"]["cpu_protocol"]["execution"]
            operation = "cpu"
            identity = {"operation": execution["operation"], "work": ActionIdentity.work_identity(root["work"])}
        else:
            operation = "action"
            identity = {key: value for key, value in ActionIdentity.record(root, group).items() if key != "group"}
        if record["operation"] != operation or record["identity"] != identity:
            raise ValueError("instantiated graph differs from its consumed execution condition")
        ports = {(port["name"], port["direction"]): row["event_ids"][port["event"]] for port in root["ports"]}
        if operation == "action":
            bindings = self.bindings.get((group, record["source"]["scope"]), {})
            if root["work"]["bindings"] != bindings or contents.invocation_bindings(0, group) != bindings:
                raise ValueError("instantiated graph changes its native invocation binding")
            self.counts["bound_invocations"] += bool(bindings)
            for boundary in self.ports[group, record["source"]["scope"]]:
                event = row["event_ids"][contents.boundary_event(0, boundary["at"])]
                self.boundaries[event].add((self.ranks[group], boundary["event"]))
            self.measured_graph = (self.ranks[group], record["source"]["scope"], contents, row["event_ids"], {}, row["work_ns"])
        else:
            begin, end = self.cpu_times[group, tuple(record["source"]["post"]), record["source"]["phase"]]
            self.boundaries[ports["call.enter", "input"]].add((None, begin))
            self.boundaries[ports["call.return", "output"]].add((None, end))
        return record["source"]["scope"] if operation == "action" else None

    def check_measured_graph(self, rank, root, target, event_ids, matches, durations):
        source, raw, _ = self.sources[rank]
        native = ActionBoundaries(source, root)
        predicted = ActionBoundaries(target, 0, event_ids)
        if matches.keys() != set(event_ids) or len(set(matches.values())) != len(event_ids):
            raise ValueError(f"rank {rank} action {root} lacks one-to-one measured event coverage")
        owned = {event for scope in source.paths(root) for event in source.events[scope]} | set(native.events.values())
        owned.update(event for _, begin, end in native.work for event in (begin, end))
        if set(matches.values()) - owned:
            raise ValueError(f"rank {rank} action {root} consumed an unrelated native event")
        if {name: matches[event] for name, event in predicted.events.items()} != native.events:
            raise ValueError(f"rank {rank} action {root} changes measured semantic ports")

        def work(boundaries, mapped):
            return Counter((identity[:2], mapped(begin), mapped(end)) for identity, begin, end in boundaries.work)

        if work(predicted, matches.__getitem__) != work(native, lambda event: event):
            raise ValueError(f"rank {rank} action {root} changes measured work endpoint pairs")
        if rank not in self.service_pairs:
            self.service_pairs[rank] = self.case.measured_service_pairs(rank, source.graph, raw)
        for piece, duration in zip(target.graph["work"], durations, strict=True):
            pair = tuple(matches[event_ids[piece[side]]] for side in ("start", "finish"))
            if (duration is not None) != (pair in self.service_pairs[rank]):
                raise ValueError(f"rank {rank} action {root} changes isolated work service evidence")
            if duration is not None:
                self.service_durations.add((rank, *pair, duration))
                self.counts["isolated_observed_services"] += 1
        self.counts.update(full_graphs=1, full_events=len(matches), full_work=len(native.work))

    def finish(self):
        if self.expected is None:
            return {"status": "not_used", "verified_prefix": False,
                    "scope": "No consumed-condition witness; ordinary logical pairing only."}
        remaining = sum(len(records) - self.positions[group] for group, records in self.expected.items())
        complete = not remaining and not self.pending and not self.reads and not self.boundaries
        clock = self.realization.solve()
        if self.logical_times:
            derived = self.case.logical_times(clock)
            for rank, event, raw_ns in self.logical_times:
                if derived.get((rank, event)) != raw_ns:
                    raise ValueError(f"rank {rank} elided wait {event} changes its known prerequisite time")
        for rank, start, finish, duration in self.service_durations:
            if clock.timestamp(rank, finish) - clock.timestamp(rank, start) != duration:
                raise ValueError(f"rank {rank} interval {start}..{finish} changes its isolated service time")
        return {"status": "complete" if complete else "partial", "verified_prefix": True,
                "full_node_coverage": complete and 0 < self.counts["full_graphs"] == self.counts["action"] and not self.counts["cpu"],
                "checked": dict(self.counts), "unconsumed": remaining, "unbound_graphs": len(self.pending), "unreturned_reads": len(self.reads),
                "unfinished_measured_boundaries": len(self.boundaries), "clock_realization": clock.report(),
                "scope": "Consumed identities and native references checked in strict CPU order; actual named completions and CPU protocol boundaries checked against original measurements and one shared clock realization whose only knots are independent calibration snapshots. No cost accuracy or independent target generalization claim."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--graphs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--layouts", type=Path, help="require every action endpoint from markov action-layouts")
    args = parser.parse_args()
    raise SystemExit(NativeInputs(args.capture, args.graphs, args.output, args.layouts).run())


if __name__ == "__main__":
    main()
