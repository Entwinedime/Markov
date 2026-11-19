"""Verify a serving structure against one observed native case."""

import argparse
import json
from pathlib import Path
from subprocess import TimeoutExpired
from types import SimpleNamespace

from markov_adapters.artifacts import write_json
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
        stages = {
            "capture": str(capture), "reference": str(args.reference.resolve(strict=True)),
            "production": str(args.production.resolve()),
        }
        checker = ProductionEvidence(args.reference, capture, self.output / "source-evidence", args.details)
        reference, evidence = checker.inspect()
        stages["reference_status"] = evidence["native_case"]["status"]
        if checker.publish(evidence):
            return self.finish("reference_unqualified", stages)

        source = json.loads(args.input.read_text())
        if difference := reference.input_difference(source):
            return self.finish("input_mismatch", stages | {"input_difference": difference})

        timing = self.output / "timing"
        extracted = NativeInputs(capture, reference.graphs, timing, args.layouts).run(reference)
        stages["timing_status"] = extracted["status"]
        if extracted["status"] != "extracted":
            return self.finish("timing_unavailable", stages)

        sources = SourceProductions(args.production)
        stages["source_observation_reused"] = any(capture.is_relative_to(path.resolve()) for path in sources.captures.values())
        prediction = self.output / "prediction"
        command = SimpleNamespace(input=args.input, production=args.production, output=prediction, timeout=args.timeout,
                                  binary=args.binary, modeling_image=args.modeling_image, control_image=args.control_image,
                                  execution_inputs=timing / "execution-inputs.json", costs=None, execution_trace=True)
        try:
            PredictionProcess(command).run(source=source, productions=sources)
        except (RuntimeError, TimeoutError, TimeoutExpired) as error:
            stages["prediction_error"] = str(error)
        trace = prediction / ("execution.partial.jsonl" if "prediction_error" in stages else "execution.jsonl")
        if not trace.is_file():
            return self.finish("prediction_no_trace", stages)

        # Compare against the original reference, including actual condition consumption and independent causal evidence.
        report = ActionComparison(reference, trace, self.output / "comparison", args.details).run()
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
    parser.add_argument("--layouts", type=Path, help="observed action layouts for full-node timing")
    parser.add_argument("--timeout", type=float, required=True, help="prediction allowance in seconds")
    parser.add_argument("--control-image", default="markov-next-control:ubuntu22.04")
    parser.add_argument("--modeling-image", default="markov-next-modeling:ubuntu24.04")
    parser.add_argument("--details", action="store_true", help="retain all evidence findings")
    raise SystemExit(0 if StructureVerification(parser.parse_args()).run() else 1)


if __name__ == "__main__":
    main()
