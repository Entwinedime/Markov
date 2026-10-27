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
    args = parser.parse_args()

    try:
        request = PredictionRequest.load(args.request)
        result = plan_prediction(request)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"markov: {error}", file=sys.stderr)
        return 2

    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
