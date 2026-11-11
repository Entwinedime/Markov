"""Extract control-feedback times from a closed native case, independently of templates."""

import argparse
import json
from collections import Counter, defaultdict
from pathlib import Path

from actions import ActionIdentity, ActionScopes
from dependencies import CausalGraph
from ingress import IngressEvidence
from protocol_reads import PdStateEvidence


class GraphTimes:
    def __init__(self, graph, group, origin_ns):
        self.graph, self.group, self.origin_ns = graph, group, origin_ns
        self.scopes = ActionScopes(graph)
        self.repeatable_reads = 0
        self.unanchored_calls = 0

    def check_queries(self, raw):
        producers = {}
        for observed in raw["scopes"]:
            scope = self.graph["scopes"][self.scopes.source_ids[observed["id"]]]
            outputs = {port["name"]: port["event"] for port in scope["ports"]
                       if port["use"] == "signal" and port["direction"] == "output"}
            for port in observed["ports"]:
                if port["use"] == "signal" and port["direction"] == "output" and "point" in port:
                    event = outputs[port["name"]]
                    previous = producers.setdefault(port["point"], event)
                    if previous != event:
                        raise ValueError("one native signal generation has different projected completion events")

        for observed in raw["scopes"]:
            scope = self.graph["scopes"][self.scopes.source_ids[observed["id"]]]
            if scope["role"] != "kv_reservation":
                continue
            ready, = [port for port in scope["ports"] if port["name"] == "reservation.ready"]
            entered = next(port["event"] for port in scope["ports"] if port["name"] == "call.enter")
            declaration, = [port for port in observed["ports"] if port["name"] == "reservation.ready"]
            if (ready["event"] != entered or ready["direction"] != "output" or ready["use"] != "signal"
                    or declaration["boundary"] != {"scope": observed["id"], "interval": "call", "side": "begin"}):
                raise ValueError("reservation readiness must refer to the actual send-entry boundary")

        # Caller effects are checked by ActionScopes; timing qualification additionally needs valid RAW intervals.
        for query in raw["state_queries"]:
            begin, end = query["begin_raw_ns"], query["end_raw_ns"]
            if begin < self.origin_ns or begin > end:
                raise ValueError("query requires an ordered RAW-clock interval after workload origin")

        # Query counts need not match across runs; each observation must agree with this source's own clock.
        for query in raw["state_queries"]:
            if query["kind"] != "event":
                continue
            begin, end = query["begin_raw_ns"], query["end_raw_ns"]
            event = producers[query["point"]]
            measured = self.graph["evidence"]["events"][event]["time_ns"]
            if measured is None:
                raise ValueError("queried signal has no measured completion")
            if (query["result"] and measured > end) or (not query["result"] and measured < begin):
                raise ValueError(f"signal point {query['point']} completion {measured} ns contradicts "
                                 f"query result {query['result']} during {begin}..{end} ns")

    def ambiguous_enclosures(self, raw):
        identities = {
            index: ActionIdentity.timing_key({**self.scopes.record(index, self.group), "site": scope["site"]})
            for index, scope in enumerate(self.graph["scopes"]) if any(raw["scopes"][origin].get("envelope", False) for origin in scope["bindings"]["source_scopes"])
        }
        counts = Counter(identities.values())
        ambiguous = set()
        for index, scope in enumerate(self.graph["scopes"]):
            if counts[identities.get(index)] > 1 or scope["parent"] in ambiguous:
                ambiguous.add(index)
        return ambiguous

    def actions(self, raw):
        graph = self.graph
        evidence = graph["evidence"]
        if evidence["clock"] != "monotonic_raw_ns" or len(evidence["events"]) != len(graph["events"]):
            raise ValueError("graph requires complete RAW-clock event evidence")
        self.check_queries(raw)
        ambiguous = self.ambiguous_enclosures(raw)
        actions, occurrences = {}, defaultdict(list)
        times, labels = {}, {}
        for index, scope in enumerate(graph["scopes"]):
            action = scope["role"]
            if not ActionIdentity.selected(index, scope):
                continue
            identity = self.scopes.record(index, self.group)

            # CPU returns release native callers; exported signals release asynchronous queries and waits.
            boundaries = {"control": [], "completion": []}
            call_times = {}
            for port in scope["ports"]:
                name = port["name"]
                if name not in {"call.enter", "call.return"} and not (port["use"] == "signal" and port["direction"] == "output"):
                    continue
                measured = evidence["events"][port["event"]]["time_ns"]
                if measured is None:
                    raise ValueError(f"scope {index} ({action}) boundary {name} is not measured")
                time_ns = measured - self.origin_ns
                if time_ns < 0:
                    raise ValueError(f"scope {index} ({action}) boundary {name} precedes workload origin")
                boundary = {"port": name, "direction": port["direction"], "time_ns": time_ns}
                if name == "call.return":
                    boundaries["control"].append(boundary)
                if port["use"] == "signal" and port["direction"] == "output":
                    boundaries["completion"].append(boundary)
                times[port["event"]] = time_ns
                labels[port["event"]] = f"scope {index} {action}/{name}"
                if name in {"call.enter", "call.return"}:
                    call_times[name] = time_ns
            if call_times.keys() != {"call.enter", "call.return"} or call_times["call.enter"] > call_times["call.return"]:
                raise ValueError(f"scope {index} ({action}) lacks an ordered call interval")
            for basis, values in boundaries.items():
                if basis == "completion" and not values:
                    continue
                work = identity["work"]
                if basis == "completion":

                    # A producer's completion is independent of ancestor batch snapshots; its own work stays exact.
                    work = {**work, "observation_context": None, "callers": [
                        {name: value for name, value in caller.items() if name != "context"}
                        for caller in work["callers"]
                    ]}
                record = {**identity, "basis": basis, "work": work}
                key = ActionIdentity.timing_key(record)
                occurrences[key].append(index)
                actions[key] = {**record, "boundaries": values}
        if not actions:
            raise ValueError("graph has no native actions")

        # Every observed call must be causal, even when repetition prevents supplying its time to the target.
        if conflict := CausalGraph.observed(graph).time_conflict(times):
            source, target = conflict
            raise ValueError(f"native feedback times contradict source dependencies: {labels[source]} at {times[source]} ns "
                             f"precedes {labels[target]} at {times[target]} ns ({times[source] - times[target]} ns inversion)")

        # Each basis has its own uniqueness domain; several placements cannot provide one guessed completion.
        for key, action in actions.items():
            indices = occurrences[key]
            action["observed_calls"] = len(indices)
            repeatable = action["action"] == "query"
            anchored = ActionIdentity.request_work(action)
            qualified = anchored and not repeatable and len(indices) == 1
            if action["basis"] == "control":
                self.repeatable_reads += len(indices) if repeatable else 0
                self.unanchored_calls += len(indices) if not anchored else 0

                # CPU returns retain the actual caller position, even when a logical producer is unique.
                missing_placement = action["action"] in {"launch", "output_prepare", "output_skip"} and action["work"]["observation_context"] is None
                qualified &= not missing_placement and not any(index in ambiguous for index in indices)
            if not qualified:
                action["boundaries"] = []
        return list(actions.values())


class NativeTiming:
    def __init__(self, capture, graphs, output):
        self.capture, self.graphs, self.output = capture, graphs, output

    @staticmethod
    def read(path):
        return json.loads(path.read_text())

    def run(self):
        capture = self.read(self.capture / "capture.json")
        actions, input_reads, ingress, origins, reports = [], [], {}, set(), []
        protocol = PdStateEvidence()
        for index, instance in enumerate(capture["instances"]):
            directory = self.capture / "instances" / str(index)
            identity = instance["id"]
            try:
                session = self.read(directory / "session.json")
                delivery, = self.read(directory / "request-delivery.json")["capture"]
                if delivery.get("clock") != "monotonic_raw_ns":
                    raise ValueError("request delivery is not on the RAW clock")
                origin_ns = delivery["origin_ns"]
                origins.add(origin_ns)
                expected = session["native_participants"]
                if set(instance["observed_roles"]) != set(expected):
                    raise ValueError("native timing requires every participant, not representative timing copies")
                requests = {request["id"]: request for request in session["content"]["requests"]}
                aliases = dict(zip(session["source_requests"], requests, strict=True))
            except (OSError, ValueError, KeyError, TypeError) as error:
                reports.append({"instance": identity, "status": "unverified", "error": str(error)})
                continue

            entries = []
            for rank in expected:
                group = f"{identity}/{rank}"
                try:
                    raw = self.read(self.capture / f"rank-{rank}.json")
                    graph = self.read(self.graphs / f"{rank}-graph.json")
                    timing = GraphTimes(graph, group, origin_ns)
                    timing.scopes.validate_source(raw, rank)
                    protocol.observe(raw, group)
                    received = IngressEvidence.rounds(raw["request_receive_rounds"], aliases, requests, origin_ns)
                    input_attempts = None
                    if raw["request_ingress"]:
                        entries.append(IngressEvidence.rounds(raw["request_ingress"], aliases, requests, origin_ns))
                        input_attempts = IngressEvidence.attempts(raw)
                        input_reads.extend(IngressEvidence.feedback(IngressEvidence.reads(identity, raw, aliases, origin_ns)))
                    observed = timing.actions(raw)
                    actions.extend(observed)
                    unconstrained = sum(action["observed_calls"] for action in observed if not action["boundaries"])
                    reports.append({"group": group, "status": "partial" if unconstrained else "extracted", "timing_records": len(observed),
                                    "state_queries": {kind: sum(query["kind"] == kind for query in raw["state_queries"])
                                                      for kind in ("event", "transport", "metadata")},
                                    "boundaries": sum(len(action["boundaries"]) for action in observed),
                                    "unconstrained_records": unconstrained,
                                    "repeatable_reads": timing.repeatable_reads, "unanchored_calls": timing.unanchored_calls,
                                    "received": received, "input_attempts": input_attempts})
                except (OSError, ValueError, KeyError, IndexError, TypeError) as error:
                    reports.append({"group": group, "status": "unverified", "error": str(error)})
            if len(entries) == 1:
                ingress[identity] = entries[0]
            else:
                reports.append({"instance": identity, "status": "unverified", "error": "expected one observed external ingress"})

        if len(origins) > 1:
            reports.append({"status": "unverified", "error": "instances use different workload time origins"})
        try:
            protocol_report = protocol.inspect()
        except (ValueError, KeyError, IndexError, TypeError) as error:
            protocol_report = {"status": "unverified", "error": str(error)}
            reports.append({"status": "unverified", "error": "PD completion evidence: " + str(error)})
        usable = (bool(reports) and len(origins) == 1
                  and all(row["status"] in {"extracted", "partial"} for row in reports)
                  and any(action["boundaries"] for action in actions))
        complete = (usable and all(row["status"] == "extracted" for row in reports)
                    and all(row["input_attempts"] is None or row["input_attempts"]["status"] == "observed" for row in reports)
                    and all(row["time_ns"] is not None for row in input_reads)
                    and not protocol_report["missing_publications"] and not protocol_report["missing_reservation_updates"]
                    and not protocol_report["missing_completion_updates"] and not protocol_report["missing_success_reads"])
        self.output.mkdir(parents=True)
        report = {
            "capture": str(self.capture.resolve()), "graphs": str(self.graphs.resolve()),
            "status": "extracted" if complete else "partial" if usable else "unverified", "origin_ns": sorted(origins), "participants": reports,
            "protocol_readiness": protocol_report,
            "scope": (
                "CPU returns retain full context and placement. Completion retains producer work and caller entry "
                "operands, excluding ancestor scheduling snapshots. Only unique request-owned work supplies times. "
                "Input reads and notifications preserve measured bounds; no batches, query values or allocations "
                "are supplied. Partial timing does not establish a unique trajectory; internal structure and "
                "cross-rank protocols need independent checks."
            ),
        }
        (self.output / "boundary-times-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        if usable:
            origin_ns, = origins
            notifications = [
                {"identity": {"group": row["group"], "name": name, "work": {"request": row["request"]}},
                 "begin_ns": row[field]["begin_raw_ns"] - origin_ns, "end_ns": row[field]["end_raw_ns"] - origin_ns}
                for row in protocol_report["endpoints"]
                for name, field in (("kv_ready", "reservation_update"), ("kv_success", "completion_update"))
                if row[field] is not None and (name != "kv_ready" or row["remote_reservation"])
            ]
            (self.output / "boundary-times.json").write_text(json.dumps({
                "actions": actions, "ingress": ingress, "notifications": notifications, "input_reads": input_reads,
            }) + "\n")
        return 0 if complete else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True, help="closed native case with all workers observed")
    parser.add_argument("--graphs", type=Path, required=True, help="directory containing <rank>-graph.json projected with evidence")
    parser.add_argument("--output", type=Path, required=True, help="new extraction directory; partial timing returns nonzero, invalid evidence publishes only a report")
    arguments = parser.parse_args()
    raise SystemExit(NativeTiming(arguments.capture, arguments.graphs, arguments.output).run())


if __name__ == "__main__":
    main()
