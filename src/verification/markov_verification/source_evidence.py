"""Check retained source evidence independently of template production and prediction."""

import argparse
from bisect import bisect_right
import json
from collections import Counter, defaultdict
from heapq import heappop, heappush
from pathlib import Path

from markov_adapters.sglang.pipeline import PipelineABI

from .actions import ActionScopes
from .native_case import NativeCase
from .source_effects import NativeEffectEvidence


class SourceEvidence:
    def __init__(self, facts, graph, clock, logical, details=False):
        self.facts, self.graph = facts, graph
        self.evidence = graph["evidence"]
        self.details = details
        self.counts = Counter()
        self.findings = []
        self.maximum_inversion_ns = 0
        self.coverage = {}
        self.clock, self.logical = clock, logical

    @staticmethod
    def require(condition, message):
        if not condition:
            raise ValueError(message)

    def coordinate(self, anchor):
        span = self.facts["spans"][anchor["span"]]
        self.require(span["begin"] <= anchor["order"] <= span["end"], f"anchor outside span: {anchor}")
        resource = span["resource"]

        # Serial boundaries can be shared by adjacent spans; concurrent services retain their own span identity.
        return resource, anchor["order"], None if self.facts["resources"][resource]["serial"] else anchor["span"]

    def validate_mapping(self):
        facts, graph, evidence = self.facts, self.graph, self.evidence
        self.require(len(graph["events"]) == len(evidence["events"]), "event evidence count differs")
        self.require(len(graph["work"]) == len(evidence["work_spans"]), "work evidence count differs")
        self.require(len(graph["scopes"]) == len(facts["scopes"]), "source scope count differs")
        self.require(graph["environment"] == facts["environment"], "source environment differs")
        self.require(graph["resources"] == [
            {key: value for key, value in resource.items() if key not in {"observed", "payload"}}
            for resource in facts["resources"]
        ], "source resource descriptors differ")
        measurements = facts["measurements"]
        self.require(all(evidence[key] == measurements[key] for key in ("clock", "window_ns")), "measurement clock/window differs")
        times = {(item["resource"], item["order"]): item["time_ns"] for item in measurements["coordinates"]}
        coordinates = {}
        for index, (event, anchor) in enumerate(zip(graph["events"], evidence["events"], strict=True)):
            key = self.coordinate(anchor)
            self.require(key not in coordinates, f"duplicate projected coordinate: {key}")
            coordinates[key] = index
            self.require(event["resource"] == key[0], f"event {index} changes source resource")
            self.require(anchor["time_ns"] == times[key[:2]], f"event {index} changes source measurement")

        edges = {(edge["source"], edge["target"], edge["kind"]) for edge in graph["edges"]}
        native_edges = set()
        for index, edge in enumerate(facts["dependencies"]):
            source, target = (coordinates[self.coordinate(edge[side])] for side in ("source", "target"))
            self.require(source == target or (source, target, edge["kind"]) in edges, f"native dependency {index} is missing")
            if source != target:
                native_edges.add((source, target, edge["kind"]))
        orders = self.validate_coverage(coordinates)
        self.require(edges == native_edges | orders, "projected dependencies omit or add source/serial relations")
        self.require(len(edges) == len(graph["edges"]), "projected dependencies repeat")

        # Projection may commute binary terms, but must preserve every declared formula and condition.
        transformed = {"arguments", "guards", "ports", "kv_accesses", "slot_accesses"}
        for index, (native, projected) in enumerate(zip(facts["scopes"], graph["scopes"], strict=True)):
            self.require({key: value for key, value in native.items() if key not in transformed} ==
                         {key: value for key, value in projected.items() if key not in transformed},
                         f"scope {index} changes source identity, context or communication")
            native_expressions = ActionScopes.declaration_expressions(native)
            projected_expressions = ActionScopes.declaration_expressions(projected)
            for field in native_expressions:
                self.require(native_expressions[field] == projected_expressions[field], f"scope {index} changes source {field}")
            self.require(len(native["ports"]) == len(projected["ports"]), f"scope {index} port count differs")
            for port, target in zip(native["ports"], projected["ports"], strict=True):
                expected = {key: value for key, value in port.items() if key not in {"at", "wait", "rendezvous"}}
                expected["event"] = coordinates[self.coordinate(port["at"])]
                if "wait" in port:
                    expected["wait"] = {
                        "resource": port["wait"]["resource"], "begin": coordinates[self.coordinate(port["wait"]["begin"])],
                    }
                if "rendezvous" in port:
                    expected["rendezvous"] = coordinates[self.coordinate(port["rendezvous"])]

                if "tensor" in expected:
                    self.require(ActionScopes.expression_list_key(expected["tensor"].get("shape_rule"))
                                 == ActionScopes.expression_list_key(target["tensor"].get("shape_rule")),
                                 f"scope {index} port {port['name']} changes its declared shape rule")
                observed = dict(target)
                for value in (expected, observed):
                    if "tensor" in value:
                        value["tensor"] = {key: item for key, item in value["tensor"].items() if key != "shape_rule"}
                self.require(expected == observed, f"scope {index} port {port['name']} differs")
            for field in ("kv_accesses", "slot_accesses"):
                self.require(len(native[field]) == len(projected[field]), f"scope {index} {field} count differs")
                for access, target in zip(native[field], projected[field], strict=True):
                    self.require(ActionScopes.expression_key(access.get("rule")) == ActionScopes.expression_key(target.get("rule")),
                                 f"scope {index} {field} changes its declared token rule")
                    expected = {key: value for key, value in access.items() if key != "rule"}
                    expected.update({side: coordinates[self.coordinate(access[side])] for side in ("start", "finish")})
                    observed = {key: value for key, value in target.items() if key != "rule"}
                    self.require(expected == observed, f"scope {index} {field} changes source access")

    def validate_coverage(self, coordinates):
        ancestors = []
        for index, scope in enumerate(self.facts["scopes"]):
            parent = scope["parent"]
            self.require((parent is None and index == 0) or (parent is not None and 0 <= parent < index),
                         f"scope {index} has an invalid source parent")
            ancestors.append({index} | (ancestors[parent] if parent is not None else set()))

        spans, boundaries, touching = defaultdict(list), defaultdict(list), defaultdict(set)
        required = set()
        for index, span in enumerate(self.facts["spans"]):
            resource = self.facts["resources"][span["resource"]]
            owner = None if resource["serial"] else index
            spans[span["resource"], owner].append((index, span))
            for position in (span["begin"], span["end"]):
                required.add((span["resource"], position, owner))
                touching[span["resource"], position, owner].add(span["scope"])
        for scope in self.facts["scopes"]:
            for port in scope["ports"]:
                required.add(self.coordinate(port["at"]))
                if "wait" in port:
                    required.add(self.coordinate(port["wait"]["begin"]))
                if "rendezvous" in port:
                    required.add(self.coordinate(port["rendezvous"]))
            for access in scope["kv_accesses"] + scope["slot_accesses"]:
                required.update(self.coordinate(access[side]) for side in ("start", "finish"))
        for edge in self.facts["dependencies"]:
            required.update(self.coordinate(edge[side]) for side in ("source", "target"))
        self.require(required == coordinates.keys(), "projected boundaries omit or add source coordinates")

        for (resource, position, owner), event in coordinates.items():
            boundaries[resource, owner].append((position, event))

        expected, orders = Counter(), set()
        for key, points in boundaries.items():
            resource = self.facts["resources"][key[0]]
            payload = resource["payload"]
            payload_ends = [end for _, end in payload]
            pending = iter(sorted(spans[key], key=lambda item: item[1]["begin"]))
            candidate = next(pending, None)
            active = []
            points.sort()
            preceding = None
            for position, (begin, start) in enumerate(points):
                while candidate is not None and candidate[1]["begin"] <= begin:
                    index, span = candidate

                    # Innermost span owns work; a nested wait suppresses its parent's service cost.
                    heappush(active, (span["end"], -span["begin"], -span["scope"], span["kind"] != "wait", -index))
                    candidate = next(pending, None)
                while active and active[0][0] <= begin:
                    heappop(active)
                index = -active[0][-1] if active else None
                span = self.facts["spans"][index] if index is not None else None

                # A shared boundary belongs to the common ancestor of both sides and all touching spans.
                owners = touching[key[0], begin, key[1]].copy()
                if preceding is not None:
                    owners.add(preceding)
                if span is not None:
                    owners.add(span["scope"])
                common = set.intersection(*(ancestors[owner] for owner in owners))
                self.require(self.graph["events"][start]["scope"] == max(common), f"event {start} changes source semantic owner")
                preceding = span["scope"] if span is not None else None
                if position + 1 == len(points):
                    continue
                end, finish = points[position + 1]
                has_work = False
                if span is not None:
                    self.require(end <= span["end"], f"resource {key} skips an active span boundary")
                    payload_index = bisect_right(payload_ends, begin)
                    has_payload = payload_index < len(payload) and payload[payload_index][0] < end
                    has_work = span["kind"] not in {"wait", "boundary"} and (span["kind"] == "host" or has_payload)
                if has_work:
                    expected[index, span["scope"], key[0], span["kind"], start, finish] += 1
                else:
                    orders.add((start, finish, "order"))

        # Compare the whole partition, not just the pieces that survived projection.
        actual = Counter((span, work["scope"], work["resource"], work["kind"], work["start"], work["finish"])
                         for span, work in zip(self.evidence["work_spans"], self.graph["work"], strict=True))
        self.require(actual == expected,
                     f"source work coverage differs: {sum((expected - actual).values())} missing, {sum((actual - expected).values())} extra")
        self.coverage = {"native_spans": len(self.facts["spans"]), "work_intervals": sum(expected.values()),
                         "ordering_intervals": len(orders)}
        return orders

    def timing(self, kind, index, source, target):
        missing = {event for event in (source, target) if self.evidence["events"][event]["time_ns"] is None}
        if missing and missing <= self.logical:
            self.counts["logical_condition_relations"] += 1
            return
        self.clock.constrain(kind, index, source, target)
        begin, end = (self.evidence["events"][event]["time_ns"] for event in (source, target))
        issue = "unknown_time" if begin is None or end is None else "time_inversion" if begin > end else None
        if issue is None:
            return
        self.counts[issue] += 1
        difference = None if issue == "unknown_time" else begin - end
        if difference is not None:
            self.maximum_inversion_ns = max(self.maximum_inversion_ns, difference)
        if self.details or self.counts[issue] <= 3:
            self.findings.append({
                "issue": issue, "kind": kind, "index": index, "source": source, "target": target,
                "source_ns": begin, "target_ns": end, "difference_ns": difference,
            })

    def inspect(self):
        self.validate_mapping()

        # The case clock already constrains measured paths through independently verified logical gates.
        for index, edge in enumerate(self.graph["edges"]):
            self.timing(edge["kind"], index, edge["source"], edge["target"])
        for index, work in enumerate(self.graph["work"]):
            self.timing("work/" + work["kind"], index, work["start"], work["finish"])
        return {
            "mapping": "consistent", "timing": self.clock.report(),
            "events": len(self.graph["events"]), "work": len(self.graph["work"]),
            "coverage": self.coverage,
            "counts": dict(self.counts), "maximum_inversion_ns": self.maximum_inversion_ns, "findings": self.findings,
        }


class ProductionEvidence:
    def __init__(self, production, capture, output, details):
        self.production, self.capture, self.output, self.details = production, capture, output, details

    def observe_rank(self, native_case, rank, facts, retain_measurements):

        # SDK/facts belong to this rank; verification can retain the independently checked service evidence.
        graph = json.loads((self.production / f"work/{rank}-graph.json").read_text())
        native = json.loads((self.capture / f"rank-{rank}.json").read_text())
        SourceEvidence.require(native["environment"] == facts["environment"], "clock source environment differs")
        native_evidence = NativeEffectEvidence(
            self.capture, rank, native, facts, self.production / f"work/{rank}-sdk/trace.json",
        )
        effects = native_evidence.inspect(graph)
        host_fences = native_evidence.inspect_host_fences(graph)
        effects["host_fences"] = len(host_fences)
        contents = native_case.observe(rank, graph, native, logical_coordinates=native_evidence.logical_coordinates)
        measurements = native_evidence.inspect_measurements()
        measurements["checked_device_measurements"] = effects["device_measurements"]
        device_intervals = native_evidence.device_intervals()
        boundaries = native_evidence.inspect_boundaries(graph, device_intervals)
        services, private_streams = native_evidence.inspect_services()
        submissions, links = native_evidence.inspect_submissions()
        launches = {} if retain_measurements else None
        payload_times = defaultdict(list) if retain_measurements else None
        payload = native_evidence.inspect_payload(graph, private_streams, device_intervals, links, launches, payload_times)
        generations = native_evidence.event_generations()
        signals = native_evidence.inspect_signals(contents, native_case.dependencies[rank], generations)
        stream_roles = native_evidence.inspect_stream_roles(*PipelineABI.stream_declarations(native), generations[1])
        report = SourceEvidence(
            facts, graph, native_case.clocks[rank], native_case.logical[rank], self.details,
        ).inspect()
        report["native_effects"] = effects
        report["native_stream_roles"] = stream_roles
        report["native_payload"] = payload
        report["native_measurements"] = measurements
        report["native_boundaries"] = boundaries
        report["native_services"] = services
        report["native_submissions"] = submissions
        report["native_signals"] = signals
        if retain_measurements:
            from .fixed_context import FixedHostContext

            records = {key: facts[key] for key in ("resources", "environment", "measurements")}
            context = FixedHostContext(self.capture, rank, native_evidence)
            delays, resumptions, queues = context.project(graph, self.details)
            report["native_host_queues"] = queues
            records["fixed_host_delays"] = delays
            records["host_fence_service"] = {pair: context.fence_service(call, sdk_entry)
                                             for pair, (call, sdk_entry) in host_fences.items()}
            records["device_releases"] = native_evidence.sdk.release_evidence(graph, launches)
            records["device_payload_times"] = payload_times
            records["fixed_queue_resumptions"] = resumptions
            records["unbound_queue_notifications"] = queues["queue_completion_notifications"] - queues["bound_to_fixed_work"]

            # Timed work and published physical signals retain their actual measured endpoints.
            timed = {piece[side] for piece in graph["work"] for side in ("start", "finish")}
            timed.update(port["event"] for scope in graph["scopes"] for port in scope["ports"]
                         if port["use"] == "signal" and port["signal"].startswith("event/"))
            records["observer_joins"] = native_evidence.observer_joins - timed
            native_case.measurements[rank] = records
        return report

    def inspect(self, *, retain_measurements=False):
        publication = json.loads((self.production / "result.json").read_text())
        capture = json.loads((self.capture / "capture.json").read_text())
        ranks = [rank for instance in capture["instances"] for rank in instance["observed_roles"]]
        SourceEvidence.require(publication["observed_roles"] == ranks, "production omits or changes the captured worker set")
        native_case = NativeCase(self.capture, capture, self.production / "work")
        reports = []
        for rank in publication["observed_roles"]:
            try:
                facts = json.loads((native_case.graphs / f"{rank}-facts.json").read_text())
                report = self.observe_rank(native_case, rank, facts, retain_measurements)
            except (OSError, ValueError, KeyError, IndexError, TypeError) as error:

                # A malformed or absent source is a finding; continue the remaining independent ranks.
                report = {"mapping": "unverified", "timing": {"status": "unverified"}, "error": str(error)}
            reports.append({"rank": rank, **report})

        case_report = {"status": "unverified", "error": "one or more worker mappings are unverified"}
        if all(report["mapping"] == "consistent" for report in reports):
            try:
                native_case.finish()
                case_report = native_case.report(self.details)
            except (OSError, ValueError, KeyError, IndexError, TypeError) as error:
                case_report = {"status": "unverified", "error": str(error)}
        document = {"production": str(self.production.resolve()), "capture": str(self.capture.resolve()),
                    "ranks": reports, "native_case": case_report}
        return native_case, document

    def publish(self, document):
        reports, case_report = document["ranks"], document["native_case"]
        self.output.mkdir(parents=True)
        (self.output / "source-evidence.json").write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n")
        lines = [
            "# Source Evidence Check", "",
            "This check compares raw declarations, interpreted records, and the coarse graph. It covers the execution environment, "
            "call hierarchy and context, communication, retained rules, work and ordering intervals, boundary ownership, geometry, "
            "dependencies, state access, and agreement between measured time and graph causality. Each work interval must appear "
            "exactly once. Waits and payload-free intervals contribute ordering only. Instrumentation completeness, applicability "
            "to target configurations, template generalization, and target predictions require separate evidence.", "",
            "Native effects are checked against SDK calls, device tasks, runtime records, and queue lifecycles from the same run. "
            "SDK records or a shared native submission group identify device work. A copy without an SDK task requires evidence of a "
            "synchronous driver transfer on the original buffer and contributes only a caller interval. Explicit synchronous copies also require a "
            "measured caller interval and a dependency on the actual return fence.", "",
            "| Rank | Source mapping | Async copies / sync copies / stream waits | Clock calibration | Nominal time inversions | "
            "Largest nominal inversion (ns) |", "| --- | --- | ---: | --- | ---: | ---: |",
        ]
        labels = {
            "consistent": "Consistent", "compatible": "Compatible within calibration bounds",
            "inconsistent": "No shared calibration", "incomplete": "Missing measurements", "unverified": "Unverified",
        }
        for report in reports:
            effects = report.get("native_effects")
            native_counts = f"{effects.get('copies', 0)} / {effects.get('synchronous_copies', 0)} / {effects.get('stream_waits', 0)}" if effects is not None else "-"
            lines.append(
                f"| {report['rank']} | {labels[report['mapping']]} | {native_counts} | {labels[report['timing']['status']]} | "
                f"{report.get('counts', {}).get('time_inversion', '-')} | {report.get('maximum_inversion_ns', '-')} |"
            )
        for report in reports:
            if "error" in report:
                lines.extend(["", f"Rank {report['rank']} could not be checked. {report['error']}", ""])
            elif "native_boundaries" in report:
                boundaries, signals = report["native_boundaries"], report["native_signals"]
                measurements = report["native_measurements"]
                services = report["native_services"]
                streams, payload = report["native_stream_roles"], report["native_payload"]
                lines.extend([
                    "", f"### Rank {report['rank']}", "",
                    f"Checked {measurements['checked_host_measurements']} host timestamps and "
                    f"{measurements['checked_device_measurements']} device endpoints against raw records. "
                    f"{measurements['unmeasured_device_endpoints']} stream boundaries lack measurements, and "
                    f"{measurements['logical_wait_boundaries']} logical wait boundaries have no device submission. "
                    "Synchronous transfers retain the driver's host timestamps, and device return fences retain SDK timestamps.", "",
                    f"Checked {streams['named_streams']} named streams, {payload.get('retained_payload_tasks', 0)} retained SDK payload tasks, "
                    f"and {payload.get('opaque_service_tasks', 0)} tasks in private service queues. "
                    f"Checked {boundaries.get('host_intervals', 0)} raw host intervals, {boundaries.get('service_intervals', 0)} service intervals, "
                    f"and {boundaries.get('declared_state_accesses', 0)} declared state accesses.", "",
                    f"Observed {services['synchronous_services']} synchronous services, {services['private_queues']} private queues, "
                    f"and {services['submitted_tasks']} native submissions. The SDK evidence includes "
                    f"{signals.get('exported_completions', 0)} exported completions, {signals.get('signal_waits', 0)} waits, "
                    f"and {signals.get('true_completion_reads', 0)} completion queries that returned true. "
                    f"The {boundaries.get('derived_state_accesses', 0)} address-derived PD state accesses require pool and transfer evidence.",
                ])
        lines.extend(["", f"Native group result `{case_report['status']}`."])
        if "error" in case_report:
            lines.extend(["", case_report["error"]])
        else:
            model = case_report["model_communication"]
            payload = case_report["pd_payload"]
            lines.extend([
                "", f"Checked {model['communication_exchanges']} model communication rounds, {case_report['model_links']} cross-rank "
                f"relations, and {case_report['pd_links']} required PD relations. CPU protocol completeness is "
                f"`{case_report['cpu_protocol']['complete']}`. Observed {case_report['physical_bindings']['observed_request_bindings']} physical request bindings.",
                "", f"PD payload checks cover {payload.get('scopes_checked', 0)} transfer scopes, {payload.get('kv_accesses_checked', 0)} KV "
                f"accesses, and {payload.get('metadata_accesses_checked', 0)} metadata accesses. Registered buffers, copied bytes, and "
                "request page tables establish the model layer and K/V component at each end. Tensor values and repeated copy counts "
                "require separate evidence.",
            ])
            for finding in case_report["findings"]:
                lines.extend(["", f"`{finding['category']}` {finding.get('reason', finding['kind'])}"])
        lines.extend([
            "", "Nominal inversions use original midpoint timestamps. Each rank uses one calibration across all measured relations, "
            "including paths through logical waits. The original API and the absence of SDK tasks and driver submissions identify "
            "pure logical waits, which carry ordering only. Physical work and externally visible completions require measured times. "
            "Compatibility establishes a feasible calibration on the original graph and timestamps. Exact offsets, capture completeness, "
            "and SDK attribution require their own evidence. `unknown_time` marks a required measurement gap.",
            "", "Exit status 0 requires consistent mappings for every rank, consistent native group evidence, all required measurements, "
            "independently supported logical waits, and a feasible shared calibration. Other outcomes return 1. The group check uses "
            "native records for participant, CPU protocol, and binding evidence. The default JSON lists up to three nominal timing "
            "findings per category. `--details` includes every location.", "",
        ])
        (self.output / "source-evidence.md").write_text("\n".join(lines))
        return 0 if reports and case_report["status"] == "consistent" and all(report["mapping"] == "consistent" and report["timing"]["status"] == "compatible"
                                   for report in reports) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--production", type=Path, required=True, help="published produce output with retained intermediates")
    parser.add_argument("--capture", type=Path, required=True, help="original native source containing independent device clock samples")
    parser.add_argument("--output", type=Path, required=True, help="new report directory")
    parser.add_argument("--details", action="store_true", help="include all timing findings")
    arguments = parser.parse_args()
    check = ProductionEvidence(arguments.production, arguments.capture, arguments.output, arguments.details)
    _, document = check.inspect()
    raise SystemExit(check.publish(document))


if __name__ == "__main__":
    main()
