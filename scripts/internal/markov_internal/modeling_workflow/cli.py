"""Container-side command-line interface for the modeling workflow."""

from __future__ import annotations

import argparse
from pathlib import Path

from .artifacts import DiagnosticLevel
from .options import nonnegative_int, positive_int, workflow_options_from_args
from .workflow import WorkflowRunner


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse workflow arguments without reading them at module import time."""

    parser = argparse.ArgumentParser(description="Predict Direct HiCache I/O/control changes from one profiled base.")
    inputs = parser.add_argument_group("prediction inputs")
    inputs.add_argument(
        "--source-manifest",
        type=Path,
        action="append",
        default=[],
        help="Source profile_manifest.json. Can be repeated for multiple workloads.",
    )
    inputs.add_argument(
        "--target-config",
        type=Path,
        action="append",
        default=[],
        help="Explicit {name?, hicache} target config. Can be repeated.",
    )
    evaluation = parser.add_argument_group("optional matrix evaluation")
    evaluation.add_argument("--evaluation", action="store_true", help="Score an observed profile matrix.")
    evaluation.add_argument(
        "--profile-run-dir",
        type=Path,
        action="append",
        default=[],
        help="Evaluation-only directory containing */profile_manifest.json.",
    )
    inputs.add_argument(
        "--output-dir",
        type=Path,
        help="Output directory. Defaults beside the first source or evaluation suite.",
    )
    evaluation.add_argument("--inputs", default="", help="Evaluation-only comma-separated input_ids.")
    evaluation.add_argument("--configs", default="", help="Evaluation-only comma-separated config_ids.")
    evaluation.add_argument("--source-configs", default="", help="Evaluation-only selected base config.")
    evaluation.add_argument("--target-configs", default="", help="Evaluation-only target config subset.")
    evaluation.add_argument(
        "--base-io-model",
        action="append",
        default=[],
        metavar="CONFIG=PATH",
        help="Evaluation-only base config to one-base I/O model mapping. Repeat for a multi-base matrix.",
    )
    evaluation.add_argument("--oracle-scores", type=Path, help="Optional data-driven oracle score manifest.")

    model_inputs = parser.add_argument_group("model inputs")
    model_inputs.add_argument(
        "--hicache-io-model",
        type=Path,
        help="Explicit compact one-base HiCache I/O model JSON.",
    )

    prediction = parser.add_argument_group("prediction")
    prediction.add_argument(
        "--max-predictions",
        type=nonnegative_int,
        default=0,
        help="Maximum cache-state predictions; 0 is all.",
    )
    artifacts = parser.add_argument_group("artifacts and diagnostics")
    artifacts.add_argument(
        "--diagnostics",
        default=DiagnosticLevel.OFF.value,
        choices=tuple(level.value for level in DiagnosticLevel),
        help=("Optional diagnostics: full retains per-cell rows, C++ details, and successful model logs."),
    )

    execution = parser.add_argument_group("execution")
    execution.add_argument("--trace-threads", type=positive_int, default=1, help="C++ logical trace read/build budget.")
    execution.add_argument("--trace-file-threads", type=positive_int, default=1, help="C++ per-file parse threads.")
    execution.add_argument("--dry-run", action="store_true", help="Write the execution plan without running C++.")
    execution.add_argument("--continue-on-error", action="store_true", help="Continue after a model command fails.")
    execution.add_argument(
        "--model-run-jobs",
        type=positive_int,
        default=1,
        help="Maximum model-run subprocesses to execute concurrently.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """Execute the requested workflow inside the modeling environment."""

    args = parse_args(argv)
    return WorkflowRunner(workflow_options_from_args(args)).run()


if __name__ == "__main__":
    raise SystemExit(main())
