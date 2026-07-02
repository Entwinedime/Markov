"""统一建模 workflow CLI。"""

from __future__ import annotations

import argparse
from pathlib import Path

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
    parser.add_argument(
        "--show-workload-sequence",
        action="store_true",
        help="Execute and show workload sequence diagnostics in HiCache preflight checks.",
    )
    parser.add_argument("--emit-transition-catalog", action="store_true", help="Emit transition mismatch catalog.")
    parser.add_argument("--emit-transition-gates", action="store_true", help="Emit transition patch gate scoreboard.")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """执行统一建模 workflow。"""

    args = parse_args(argv)
    return WorkflowRunner(WorkflowOptionsBuilder().from_args(args)).run()


if __name__ == "__main__":
    raise SystemExit(main())
