"""Thin command dispatch; each workflow owns its business operations."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
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
    args = parser.parse_args()

    try:
        request = PredictionRequest.load(args.request)
        if args.command == "plan":
            result = plan_prediction(request)
        else:
            from markov.workflow.capture import BaseCapture

            result = BaseCapture(request).run(recapture=args.recapture)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"markov: {error}", file=sys.stderr)
        return 2

    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["status"] in ("planned", "completed", "reused") else 1


if __name__ == "__main__":
    raise SystemExit(main())
