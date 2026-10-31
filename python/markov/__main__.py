"""Thin command dispatch; each workflow owns its business operations."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sqlite3
import subprocess
import sys

from markov.artifacts.request import PredictionRequest
from markov.workflow.planning import plan_prediction


def main() -> int:
    parser = argparse.ArgumentParser(prog="markov", description="Plan trace-driven configuration prediction.")
    commands = parser.add_subparsers(dest="command", required=True)
    preview = commands.add_parser("plan", help="Preview inputs and preparation without executing them.")
    preview.add_argument("--request", type=Path, required=True)
    prediction = commands.add_parser(
        "predict", help="Predict all targets from one base and selected independent models."
    )
    prediction.add_argument("--request", type=Path, required=True)
    search_command = commands.add_parser("search", help="Rank a finite prediction set under declared resource plans.")
    search_command.add_argument("--request", type=Path, required=True)
    capture = commands.add_parser("capture", help="Capture the declared base using its runtime and cumulative budget.")
    capture.add_argument("--request", type=Path, required=True)
    capture.add_argument("--recapture", action="store_true", help="Collect again without resetting prior budget use.")
    capture.add_argument(
        "--record-regions",
        action="store_true",
        help="Record profiled region costs for independent evaluation; not training data.",
    )
    measure = commands.add_parser("measure", help="Measure base.configuration without profiling or probes.")
    measure.add_argument("--request", type=Path, required=True)
    measure.add_argument(
        "--recapture", action="store_true", help="Repeat normal measurement without resetting budget use."
    )
    calibration = commands.add_parser(
        "calibrate", help="Measure shared eager compute regions without profiler exports."
    )
    calibration.add_argument("--request", type=Path, required=True)
    calibration.add_argument(
        "--recapture", action="store_true", help="Repeat without resetting cumulative calibration use."
    )
    maintenance = commands.add_parser(
        "maintain", help="Import independent performance data and train selected offline responses."
    )
    maintenance.add_argument("--request", type=Path, required=True)
    replay_command = commands.add_parser(
        "replay", help="Replay a source DAG or recorded predicted path with explicit costs."
    )
    replay_command.add_argument("--request", type=Path, required=True)
    evaluation_command = commands.add_parser(
        "evaluate", help="Compare a recorded prediction with a separate real target profile."
    )
    evaluation_command.add_argument("--request", type=Path, required=True)
    args = parser.parse_args()

    try:
        if args.command == "search":
            from markov.workflow.search import search

            result = search(args.request)
        elif args.command == "evaluate":
            from markov.workflow.evaluation import evaluate

            result = evaluate(args.request)
        elif args.command == "replay":
            from markov.workflow.replay import replay

            result = replay(args.request)
        elif args.command == "maintain":
            from markov.workflow.performance import maintain_performance

            result = maintain_performance(args.request)
        else:
            request = PredictionRequest.load(args.request)
            if args.command == "plan":
                result = plan_prediction(request)
            elif args.command == "predict":
                from markov.workflow.prediction import predict

                result = predict(request)
            else:
                from markov.workflow.capture import InferenceCapture

                result = InferenceCapture(
                    request,
                    profiling=args.command == "capture",
                    calibration=args.command == "calibrate",
                    record_regions=args.command == "capture" and args.record_regions,
                ).run(recapture=args.recapture)
    except (OSError, ValueError, KeyError, TypeError, sqlite3.Error, subprocess.SubprocessError) as error:
        print(f"markov: {error}", file=sys.stderr)
        return 2

    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["status"] in ("planned", "completed", "reused") else 1


if __name__ == "__main__":
    raise SystemExit(main())
