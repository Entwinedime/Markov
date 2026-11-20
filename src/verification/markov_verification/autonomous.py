"""Verify cost-driven serving against an independent lightweight native run."""

import argparse
import json
from pathlib import Path
from subprocess import TimeoutExpired
from types import SimpleNamespace

from markov_adapters.artifacts import write_json
from markov_workflow.capture import CaptureProcess
from markov_workflow.prediction import PredictionProcess

from .latency_metrics import LatencyComparison, NativeLatencyMetrics


class AutonomousVerification:
    def __init__(self, arguments):
        self.arguments = arguments
        self.output = arguments.output.resolve()
        self.report = {
            "input": str(arguments.input.resolve()),
            "native_task": str(arguments.capture.resolve()),
            "production": str(arguments.production.resolve()),
            "cost_asset": str(arguments.costs.resolve()),
        }

    def finish(self, status):
        write_json(self.output / "result.json", {"status": status, **self.report}, indent=2, replace=False)
        return status == "compared"

    def run(self):
        args = self.arguments
        self.output.mkdir(parents=True)
        recorded = json.loads((args.capture / "execution.json").read_text())["inputs"]
        capture = CaptureProcess.published(args.capture, recorded["job"])
        source = json.loads(args.input.read_text())
        fields = ("model", "instances", "workload")
        if source.keys() != set(fields) or any(source[field] != recorded["job"][field] for field in fields):
            return self.finish("input_mismatch")
        if not recorded.get("latency_only") or recorded.get("host_timing"):
            return self.finish("reference_requires_latency_capture")

        native = NativeLatencyMetrics(capture).report()
        self.report["native"] = native
        if native["specialization_loads"] is None:
            return self.finish("reference_missing_specialization_evidence")
        if native["specialization_loads"]:
            return self.finish("reference_loaded_specializations")
        rows = native["requests"]
        if any(actual["declared_ns"] != expected["arrival_ns"]
               for actual, expected in zip(rows, source["workload"]["requests"], strict=True)):
            return self.finish("reference_changed_arrivals")

        submissions = ["submitted_ns"]
        if "decode_submitted_ns" in rows[0]:
            submissions.append("decode_submitted_ns")
        order = sorted(rows, key=lambda row: row["declared_ns"])
        self.report["max_submission_delay_ns"] = max(row[field] - row["declared_ns"] for field in submissions for row in rows)
        if any(order != sorted(rows, key=lambda row: row[field]) for field in submissions):
            return self.finish("reference_reordered_submissions")

        # Prediction receives the shared user input and independent cost asset.
        output = self.output / "prediction"
        command = SimpleNamespace(input=args.input, production=args.production, output=output, timeout=args.timeout,
                                  binary=args.binary, modeling_image=args.modeling_image, control_image=args.control_image,
                                  execution_inputs=None, costs=args.costs, execution_trace=args.details)
        try:
            PredictionProcess(command).run(source=source)
        except (RuntimeError, TimeoutError, TimeoutExpired) as error:
            self.report["prediction_error"] = str(error)
            return self.finish("prediction_failed")

        self.report["prediction"] = LatencyComparison(native, output / "result.json").report()
        return self.finish("compared")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True, help="completed lightweight native capture task")
    for name in ("production", "input", "costs", "binary", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--timeout", type=float, required=True, help="prediction allowance in seconds")
    parser.add_argument("--control-image", default="markov-next-control:ubuntu22.04")
    parser.add_argument("--modeling-image", default="markov-next-modeling:ubuntu24.04")
    parser.add_argument("--details", action="store_true", help="retain the execution trace")
    raise SystemExit(0 if AutonomousVerification(parser.parse_args()).run() else 1)


if __name__ == "__main__":
    main()
