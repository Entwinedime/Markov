"""Command-line interface for the host-side modeling workflow."""

from __future__ import annotations

import argparse
from pathlib import Path

from ..common.paths import running_in_modeling_container
from .options import nonnegative_int, positive_int, workflow_options_from_args
from .validations.registry import validation_names
from .workflow import WorkflowRunner


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse workflow arguments without reading them at module import time."""

    parser = argparse.ArgumentParser(description="Run post-profile modeling and selected validations.")
    inputs = parser.add_argument_group("profile selection")
    inputs.add_argument(
        "--profile-run-dir",
        type=Path,
        action="append",
        default=[],
        help="Directory containing */profile_manifest.json. Can be repeated.",
    )
    inputs.add_argument(
        "--manifest",
        type=Path,
        action="append",
        default=[],
        help="Explicit profile_manifest.json path. Can be repeated.",
    )
    inputs.add_argument(
        "--output-dir",
        type=Path,
        help="Output directory. Defaults to <first profile-run-dir>/modeling/modeling_workflow.",
    )
    inputs.add_argument("--inputs", default="", help="Comma-separated input_ids.")
    inputs.add_argument("--configs", default="", help="Comma-separated config_ids.")
    inputs.add_argument("--source-configs", default="", help="Comma-separated source config_ids.")
    inputs.add_argument("--target-configs", default="", help="Comma-separated target config_ids.")

    validation = parser.add_argument_group("validation")
    validation.add_argument(
        "--validations",
        required=True,
        help="Comma-separated validation objects: " + ",".join(validation_names()),
    )
    validation.add_argument(
        "--prediction-scope",
        default="self,cross",
        help="Comma-separated cache-state scopes: self,cross.",
    )
    validation.add_argument(
        "--max-predictions",
        type=nonnegative_int,
        default=0,
        help="Maximum cache-state predictions; 0 is all.",
    )
    validation.add_argument(
        "--page-key-mode",
        default="strip_scope",
        choices=("strip_scope", "raw"),
        help="HiCache validation page identity mode.",
    )
    validation.add_argument(
        "--sample",
        type=positive_int,
        default=20,
        help="Maximum HiCache transition mismatch/evidence sample size.",
    )

    execution = parser.add_argument_group("execution")
    execution.add_argument("--trace-threads", type=positive_int, default=1, help="C++ logical trace read/build budget.")
    execution.add_argument("--trace-file-threads", type=positive_int, default=1, help="C++ per-file parse threads.")
    execution.add_argument("--dry-run", action="store_true", help="Write the execution plan without running C++.")
    execution.add_argument("--force", action="store_true", help="Rebuild existing model run artifacts.")
    execution.add_argument("--continue-on-error", action="store_true", help="Continue after a model command fails.")
    execution.add_argument(
        "--model-run-jobs",
        type=positive_int,
        default=1,
        help="Maximum model-run subprocesses to execute concurrently.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """Validate the launch environment and execute the requested workflow."""

    require_host_workflow_entrypoint()
    args = parse_args(argv)
    return WorkflowRunner(workflow_options_from_args(args)).run()


def require_host_workflow_entrypoint() -> None:
    """Reject recursive launches from inside the modeling container.

    Users always start the Python entrypoint from the checkout. Individual
    model cells may enter the container through ``scripts/model.sh``, but that
    internal boundary must not start another host workflow.
    """

    if not running_in_modeling_container():
        return
    raise SystemExit(
        "modeling_workflow.py must be started from the host checkout. "
        "Use: python3 scripts/internal/entrypoints/modeling_workflow.py ..."
    )


if __name__ == "__main__":
    raise SystemExit(main())
