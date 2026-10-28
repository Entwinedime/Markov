"""Run one native-controlled prediction in isolated, device-free containers."""

import argparse
from pathlib import Path

from markov_control.workflow.session import NativeExecution


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--templates", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=float, default=300)
    parser.add_argument("--details", action="store_true")
    arguments = parser.parse_args()
    if arguments.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be positive")

    root = Path(__file__).resolve().parents[4]
    NativeExecution(root, arguments.output, arguments.timeout_seconds).run(
        arguments.templates, arguments.input, arguments.details
    )


if __name__ == "__main__":
    main()
