"""Thin command dispatch; each workflow owns its business operations."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sqlite3
import sys

from markov.artifacts.request import PredictionRequest
from markov.workflow.planning import plan_prediction


def main() -> int:
    parser = argparse.ArgumentParser(prog="markov", description="Plan trace-driven configuration prediction.")
    commands = parser.add_subparsers(dest="command", required=True)
    preview = commands.add_parser("plan", help="Preview inputs and preparation without executing them.")
    preview.add_argument("--request", type=Path, required=True)
    capture = commands.add_parser("capture", help="Capture the declared base using its runtime and cumulative budget.")
    capture.add_argument("--request", type=Path, required=True)
    capture.add_argument("--recapture", action="store_true", help="Collect again without resetting prior budget use.")
    maintenance = commands.add_parser(
        "maintain", help="Import independent performance data and train selected offline responses."
    )
    maintenance.add_argument("--request", type=Path, required=True)
    args = parser.parse_args()

    try:
        if args.command == "maintain":
            from markov.workflow.performance import maintain_performance

            result = maintain_performance(args.request)
        else:
            request = PredictionRequest.load(args.request)
            if args.command == "plan":
                result = plan_prediction(request)
            else:
                from markov.workflow.capture import BaseCapture

                result = BaseCapture(request).run(recapture=args.recapture)
    except (OSError, ValueError, KeyError, TypeError, sqlite3.Error) as error:
        print(f"markov: {error}", file=sys.stderr)
        return 2

    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["status"] in ("planned", "completed", "reused") else 1


if __name__ == "__main__":
    raise SystemExit(main())
