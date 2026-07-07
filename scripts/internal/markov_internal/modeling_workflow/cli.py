"""统一建模 workflow CLI。"""

from __future__ import annotations

import argparse
from pathlib import Path

from ..common.paths import running_in_modeling_container
from .options import WorkflowOptionsBuilder
from .validations import validation_names
from .workflow import WorkflowRunner


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """解析统一 workflow 参数。"""

    parser = argparse.ArgumentParser(description="Run the unified post-profile modeling workflow.")
    parser.add_argument(
        "--profile-run-dir",
        type=Path,
        action="append",
        default=[],
        help="Directory containing */profile_manifest.json. Can be repeated.",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        action="append",
        default=[],
        help="Explicit profile_manifest.json path. Can be repeated.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="Output directory. Defaults to <first profile-run-dir>/modeling/modeling_workflow.",
    )
    parser.add_argument(
        "--validations",
        required=True,
        help="Comma-separated validation objects: " + ",".join(validation_names()),
    )
    parser.add_argument("--input", action="append", default=[], help="Run one input_id. Can be repeated.")
    parser.add_argument("--inputs", action="append", default=[], help="Comma-separated input_ids.")
    parser.add_argument("--config", action="append", default=[], help="Run one config_id. Can be repeated.")
    parser.add_argument("--configs", action="append", default=[], help="Comma-separated config_ids.")
    parser.add_argument("--source-config", action="append", default=[], help="Run one source config_id.")
    parser.add_argument("--source-configs", action="append", default=[], help="Comma-separated source config_ids.")
    parser.add_argument("--target-config", action="append", default=[], help="Run one target config_id.")
    parser.add_argument("--target-configs", action="append", default=[], help="Comma-separated target config_ids.")
    parser.add_argument(
        "--prediction-scope",
        default="self,cross",
        help="Comma-separated cache-state scopes: self,cross.",
    )
    parser.add_argument("--dry-run", action="store_true", help="Write plans and commands without running C++.")
    parser.add_argument("--force", action="store_true", help="Rebuild existing model run artifacts.")
    parser.add_argument("--continue-on-error", action="store_true", help="Continue after a model command fails.")
    parser.add_argument("--max-predictions", type=int, default=0, help="Maximum cache-state predictions; 0 is all.")
    parser.add_argument("--page-key-mode", default="strip_scope", choices=("strip_scope", "raw"))
    parser.add_argument("--sample", type=int, default=20, help="Maximum transition mismatch/evidence sample size.")
    parser.add_argument("--trace-threads", type=int, default=1, help="C++ logical trace input read/build threads.")
    parser.add_argument("--trace-file-threads", type=int, default=1, help="C++ per-file trace parse threads.")
    parser.add_argument(
        "--model-run-jobs",
        "--parallel-jobs",
        dest="model_run_jobs",
        type=int,
        default=1,
        help="Maximum model-run subprocesses to execute concurrently.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """执行统一建模 workflow。"""

    require_host_workflow_entrypoint()
    args = parse_args(argv)
    return WorkflowRunner(WorkflowOptionsBuilder().from_args(args)).run()


def require_host_workflow_entrypoint() -> None:
    """workflow 只允许从宿主机 Python 入口启动。"""

    if not running_in_modeling_container():
        return
    raise SystemExit(
        "modeling_workflow.py must be started from the host checkout. "
        "Use: python3 scripts/internal/entrypoints/modeling_workflow.py ..."
    )


if __name__ == "__main__":
    raise SystemExit(main())
