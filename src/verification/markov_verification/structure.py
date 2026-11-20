"""Verify a serving structure against one observed native case."""

import argparse
import json
from pathlib import Path
from subprocess import CalledProcessError, TimeoutExpired
from tempfile import TemporaryDirectory
from time import monotonic_ns
from types import SimpleNamespace

from markov_adapters.artifacts import write_json
from markov_workflow.containers import ModelingProcess
from markov_workflow.prediction import PredictionProcess
from markov_workflow.source_productions import SourceProductions

from .compare_actions import ActionComparison
from .execution_inputs import NativeInputs
from .source_evidence import ProductionEvidence


class StructureVerification:
    def __init__(self, arguments):
        self.arguments = arguments
        self.output = arguments.output.resolve()

    def finish(self, status, stages):
        write_json(self.output / "result.json", {"status": status, **stages}, indent=2, replace=False)
        return status in {"full_structure_matched", "structure_matched_within_scope"}

    def run(self):
        args = self.arguments
        self.output.mkdir(parents=True)
        capture = args.capture.resolve(strict=True)
        wall_ns = {}
        stages = {
            "capture": str(capture), "reference": str(args.reference.resolve(strict=True)),
            "production": str(args.production.resolve()), "wall_ns": wall_ns,
        }
        started = monotonic_ns()
        checker = ProductionEvidence(args.reference, capture, self.output / "source-evidence", args.details)
        reference, evidence = checker.inspect()
        stages["reference_status"] = evidence["native_case"]["status"]
        status = checker.publish(evidence)
        wall_ns["reference"] = monotonic_ns() - started
        if status:
            return self.finish("reference_unqualified", stages)

        source = json.loads(args.input.read_text())
        if difference := reference.input_difference(source):
            return self.finish("input_mismatch", stages | {"input_difference": difference})

        started = monotonic_ns()
        timing = self.output / "timing"
        with TemporaryDirectory(prefix=".action-layouts-", dir=self.output) as temporary:
            layouts = Path(temporary)
            modeling = ModelingProcess(args.binary, args.modeling_image, args.timeout)
            try:
                for rank in reference.sources:
                    modeling.run_model("action-layouts", layouts / f"{rank}-layouts.json", reference.graphs / f"{rank}-graph.json")
            except (CalledProcessError, TimeoutError, TimeoutExpired, RuntimeError) as error:
                return self.finish("action_layout_unavailable", stages | {"layout_error": str(error)})
            inputs = NativeInputs(capture, reference.graphs, timing, layouts)
            extracted = inputs.run(reference)
        wall_ns["timing"] = monotonic_ns() - started
        stages["timing_status"] = extracted["status"]
        if extracted["status"] != "extracted":
            return self.finish("timing_unavailable", stages)

        sources = SourceProductions(args.production)
        stages["source_observation_reused"] = any(capture.is_relative_to(path.resolve()) for path in sources.captures.values())
        prediction = self.output / "prediction"
        command = SimpleNamespace(input=args.input, production=args.production, output=prediction, timeout=args.timeout,
                                  binary=args.binary, modeling_image=args.modeling_image, control_image=args.control_image,
                                  execution_inputs=timing / "execution-inputs.json", costs=None, execution_trace=True)
        started = monotonic_ns()
        try:
            PredictionProcess(command).run(source=source, productions=sources)
        except (RuntimeError, TimeoutError, TimeoutExpired) as error:
            stages["prediction_error"] = str(error)
        wall_ns["prediction"] = monotonic_ns() - started
        trace = prediction / ("execution.partial.jsonl" if "prediction_error" in stages else "execution.jsonl")
        if not trace.is_file():
            return self.finish("prediction_no_trace", stages)

        # Compare against the original reference, including actual condition consumption and independent causal evidence.
        started = monotonic_ns()
        try:
            report = ActionComparison(reference, trace, self.output / "comparison", args.details,
                                      realization=inputs.realization).run()
        except ValueError as error:
            wall_ns["comparison"] = monotonic_ns() - started
            return self.finish("comparison_unverified", stages | {"comparison_error": str(error)})
        wall_ns["comparison"] = monotonic_ns() - started
        stages["comparison_status"] = report["status"]
        stages["coverage"] = report["coverage"]
        stages["actions_compared"] = report["actions_compared"]
        if "prediction_error" in stages:
            return self.finish("prediction_failed", stages)
        if report["status"] != "actions_matched":
            return self.finish(report["status"], stages)
        if report["condition_inputs"]["full_node_coverage"]:
            return self.finish("full_structure_matched", stages)
        return self.finish("structure_matched_within_scope", stages)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("capture", "reference", "production", "input", "binary", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--timeout", type=float, required=True, help="allowance in seconds for each of layout preparation and prediction")
    parser.add_argument("--control-image", default="markov-next-control:ubuntu22.04")
    parser.add_argument("--modeling-image", default="markov-next-modeling:ubuntu24.04")
    parser.add_argument("--details", action="store_true", help="retain all evidence findings")
    raise SystemExit(0 if StructureVerification(parser.parse_args()).run() else 1)


if __name__ == "__main__":
    main()
