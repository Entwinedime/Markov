"""Verify service costs without changing the observed serving execution."""

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
from tempfile import TemporaryDirectory

from markov_adapters.artifacts import write_json
from markov_workflow.containers import ModelingProcess

from .fixed_reference import FixedReference
from .source_evidence import ProductionEvidence


class FixedPerformance:
    def __init__(self, arguments):
        self.arguments = arguments
        self.output = arguments.output.resolve()
        self.report = {"capture": str(arguments.capture.resolve()), "reference": str(arguments.reference.resolve()),
                       "cost_asset": str(arguments.costs.resolve())}

    def finish(self, status):
        write_json(self.output / "result.json", {"status": status, **self.report}, indent=2, replace=False)
        return status == "compared"

    def estimate(self, reference, model, scratch):
        estimates, first_gaps = {}, {}
        for rank, (contents, _, _) in reference.case.sources.items():
            output = scratch / f"{rank}-costs.json"
            model.run_model("work-costs", output, reference.case.graphs / f"{rank}-graph.json", self.arguments.costs)
            result = json.loads(output.read_text())
            estimates[rank] = result["work_ns"]
            if len(estimates[rank]) != len(contents.graph["work"]):
                raise ValueError("fixed cost response changes the observed work set")
            if result["first_cost_gap"] is not None:
                first_gaps[rank] = result["first_cost_gap"]
        self.report["first_cost_gaps"] = first_gaps
        return [estimates[interval.rank][interval.index] for interval in reference.intervals]

    def assess(self, reference, estimates):
        coverage = defaultdict(lambda: {"work": 0, "measured": 0, "modeled": 0, "paired": 0,
                                        "signed_error_ns": 0, "absolute_error_ns": 0})
        first_missing = None
        for interval, estimate in zip(reference.intervals, estimates, strict=True):
            row = coverage[interval.piece["kind"]]
            duration = interval.service_ns
            row["work"] += 1
            row["measured"] += duration is not None
            row["modeled"] += estimate is not None
            if duration is None:
                if first_missing is None:
                    first_missing = reference.detail(interval)
                continue
            if estimate is not None:
                error = estimate - duration
                row["paired"] += 1
                row["signed_error_ns"] += error
                row["absolute_error_ns"] += abs(error)
        self.report["service_coverage"] = dict(coverage)
        self.report["observation_basis"] = dict(Counter(interval.basis for interval in reference.intervals))
        self.report["first_unmeasured_work"] = first_missing
        return first_missing is None

    @staticmethod
    def first_drift(reference, plan, services, baseline, replayed):
        positions = reference.case.graph.positions
        first = reference.check_baseline(replayed)["first_difference"]
        if first is None:
            return None
        target = positions[first["rank"], first["event"]]
        tight = defaultdict(list)
        for source, consumer, lag in plan["dependencies"]:
            if (consumer == target or consumer >= len(positions)) and replayed[source] + lag == replayed[consumer]:
                tight[consumer].append(source)
        for (source, consumer, fixed), service in zip(plan["work"], services, strict=True):
            if (consumer == target or consumer >= len(positions)) and replayed[source] + service + fixed == replayed[consumer]:
                tight[consumer].append(source)

        # Auxiliary gates retain the nearest original prerequisite for attribution.
        pending = [target]
        visited = {target}
        identities = list(positions)
        predecessor = None
        for event in pending:
            predecessor = next((identities[source] for source in tight[event] if source < len(identities)), None)
            if predecessor is not None:
                break
            for source in tight[event]:
                if source not in visited:
                    pending.append(source)
                    visited.add(source)
        return {**first, "baseline_ns": baseline[target], "tight_predecessor": predecessor}

    @staticmethod
    def metric_errors(baseline, modeled):
        errors = []
        for original, replayed in zip(baseline["requests"], modeled["requests"], strict=True):
            tpot = None if original["tpot_ns"] is None else replayed["tpot_ns"] - original["tpot_ns"]
            errors.append({"id": original["id"], "ttft_ns": replayed["ttft_ns"] - original["ttft_ns"], "tpot_ns": tpot})
        actual_rate, predicted_rate = (row["tokens_per_second"] for row in (baseline, modeled))
        return {"requests": errors, "p95_ttft_ns": modeled["p95_ttft_ns"] - baseline["p95_ttft_ns"],
                "tokens_per_second": None if actual_rate is None or predicted_rate is None else predicted_rate - actual_rate}

    def replay(self, reference, model, scratch, estimates):
        measured = [interval.service_ns for interval in reference.intervals]
        plan = reference.timeline()
        path = scratch / "replay.json"
        write_json(path, plan, replace=False)
        measured_steps = reference.replay_services(measured)
        services = scratch / "measured.json"
        write_json(services, {"measured": measured_steps}, replace=False)
        output = scratch / "measured-times.json"
        model.run_model("replay", output, path, services)
        times = json.loads(output.read_text())["event_times_ns"]["measured"]
        baseline = self.report["baseline"] = reference.check_baseline(times)
        baseline["request_metrics"] = reference.metrics(times)
        if baseline["first_difference"] is not None:
            baseline["local_unexplained_waits"] = reference.local_unexplained_waits(plan, measured_steps)
            return self.finish("baseline_difference")
        if None in reference.specialization_loads.values():
            return self.finish("reference_specializations_unobserved")
        if None in estimates:
            return self.finish("cost_gap")

        scenarios = {"modeled": estimates}
        for kind in sorted(self.report["service_coverage"]):
            scenarios[f"restore_{kind}"] = [observed if interval.piece["kind"] == kind else estimate
                                             for interval, estimate, observed in zip(reference.intervals, estimates, measured, strict=True)]
        scenarios = {name: reference.replay_services(durations) for name, durations in scenarios.items()}
        services = scratch / "scenarios.json"
        write_json(services, scenarios, replace=False)
        output = scratch / "scenario-times.json"
        model.run_model("replay", output, path, services)
        replayed = json.loads(output.read_text())["event_times_ns"]
        outcomes = {}
        for name, stamps in replayed.items():
            metrics = reference.metrics(stamps)
            outcomes[name] = {"request_metrics": metrics, "delta_from_baseline": self.metric_errors(baseline["request_metrics"], metrics),
                              "first_timing_difference": self.first_drift(reference, plan, scenarios[name], times, stamps)}
        self.report["scenarios"] = outcomes
        return self.finish("compared")

    def run(self):
        args = self.arguments
        self.output.mkdir(parents=True)
        checker = ProductionEvidence(args.reference, args.capture, self.output / "source-evidence", args.details)
        case, evidence = checker.inspect(retain_measurements=True)
        self.report["reference_status"] = evidence["native_case"]["status"]
        if checker.publish(evidence) or case.groups.keys() != case.sources.keys():
            return self.finish("reference_unqualified")

        reference = FixedReference(case)
        self.report["reference_gaps"] = reference.gaps
        self.report["specialization_loads"] = reference.specialization_loads
        self.report["external_releases"] = [{"rank": rank, "event": event, "time_ns": stamp}
                                            for (rank, event), stamp in reference.entries.items()]
        conditions = Counter()
        for row in reference.host_conditions.values():
            conditions.update(row)
        self.report["fixed_host_conditions"] = {"steps": len(reference.host_conditions), **conditions}
        self.report["fixed_host_fences"] = {"qualified": len(reference.host_fences), "service_ns": sum(
            head + tail for _, head, tail in reference.host_fences.values())}
        self.report["fixed_device_dispatch"] = reference.device_dispatch
        model = ModelingProcess(args.binary, args.modeling_image, args.timeout)
        self.report["modeling_image"] = model.image
        with TemporaryDirectory(prefix=".fixed-", dir=self.output) as scratch:
            scratch = Path(scratch)
            estimated = self.estimate(reference, model, scratch)
            measured = self.assess(reference, estimated)
            if not measured or reference.gaps:
                return self.finish("reference_timing_incomplete")
            if any(reference.specialization_loads.values()):
                return self.finish("reference_loaded_specializations")
            return self.replay(reference, model, scratch, estimated)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("capture", "reference", "costs", "binary", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--modeling-image", default="markov-next-modeling:ubuntu24.04")
    parser.add_argument("--timeout", type=float, required=True, help="cumulative modeling allowance in seconds")
    parser.add_argument("--details", action="store_true", help="retain all reference evidence findings")
    raise SystemExit(0 if FixedPerformance(parser.parse_args()).run() else 1)


if __name__ == "__main__":
    main()
