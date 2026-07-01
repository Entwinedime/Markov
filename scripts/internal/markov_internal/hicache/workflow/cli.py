#!/usr/bin/env python3
"""HiCache profiling 后 validation 主入口。

该入口负责把 profile quality、final-state prediction 和 transition exactness
串成一个清晰 workflow。它只编排已有 profile 产物和 modeling 命令，不启动真实
profiling，也不修改 Python probe trace。
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any


from ...common.paths import ROOT_DIR
from .core.context import WorkflowArtifactPolicy, WorkflowRunContext
from .planning.plan import write_workflow_plan
from .output.progress import WorkflowProgressReporter
from .stages.runner import FinalStateStageRunner, QualityStageRunner, TransitionStageRunner
from .stages.final_state import FinalStateOptions
from .stages.transition import TransitionOptions
from .output.summary import workflow_mode_from_quality, write_workflow_summary
from ..matrix.runs.discovery import discover_profile_runs, filter_runs


DEFAULT_STAGES = ("quality", "final-state")
DEFAULT_PREDICTION_SCOPE = ("self", "cross")
STAGE_CHOICES = {"quality", "final-state", "transition"}
PREDICTION_SCOPE_CHOICES = {"self", "cross"}


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """解析 workflow CLI 参数。"""

    parser = argparse.ArgumentParser(description="Run HiCache post-profile validation workflow.")
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
        help="Output directory. Defaults to <first profile-run-dir>/modeling/hicache_state_workflow.",
    )
    parser.add_argument(
        "--stages",
        default=",".join(DEFAULT_STAGES),
        help="Comma-separated stages: quality,final-state,transition.",
    )
    parser.add_argument(
        "--prediction-scope",
        default=",".join(DEFAULT_PREDICTION_SCOPE),
        help="Comma-separated final-state scopes: self,cross.",
    )
    parser.add_argument("--input", action="append", default=[], help="Run one input_id. Can be repeated.")
    parser.add_argument("--inputs", action="append", default=[], help="Comma-separated input_ids.")
    parser.add_argument(
        "--source-config",
        action="append",
        default=[],
        help="Run one source config_id. Can be repeated.",
    )
    parser.add_argument("--source-configs", action="append", default=[], help="Comma-separated source config_ids.")
    parser.add_argument(
        "--target-config",
        action="append",
        default=[],
        help="Run one target config_id. Can be repeated.",
    )
    parser.add_argument("--target-configs", action="append", default=[], help="Comma-separated target config_ids.")
    parser.add_argument("--force", action="store_true", help="Rebuild existing prediction / transition artifacts.")
    parser.add_argument("--dry-run", action="store_true", help="Write plans and commands without running modeling.")
    parser.add_argument("--continue-on-error", action="store_true", help="Continue after a prediction command fails.")
    parser.add_argument(
        "--max-predictions", type=int, default=0, help="Maximum predictions to execute; 0 means no limit."
    )
    parser.add_argument("--page-key-mode", default="strip_scope", choices=("strip_scope", "raw"))
    parser.add_argument("--sample", type=int, default=20, help="Maximum mismatch / evidence sample size.")
    parser.add_argument(
        "--show-workload-sequence",
        action="store_true",
        help="Show workload identity sequence diagnostics in the quality stage summary.",
    )
    parser.add_argument(
        "--emit-transition-catalog", action="store_true", help="Emit transition mismatch catalog artifacts."
    )
    parser.add_argument("--emit-transition-gates", action="store_true", help="Emit transition patch gate artifacts.")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """CLI 入口。"""

    args = parse_args(argv)
    stages = parse_csv_set(args.stages, STAGE_CHOICES, default=DEFAULT_STAGES, label="stages")
    prediction_scope = parse_csv_set(
        args.prediction_scope,
        PREDICTION_SCOPE_CHOICES,
        default=DEFAULT_PREDICTION_SCOPE,
        label="prediction scope",
    )
    final_state_options = FinalStateOptions(
        source_config_ids=parse_selector_values(args.source_config, args.source_configs),
        target_config_ids=parse_selector_values(args.target_config, args.target_configs),
        prediction_scope=prediction_scope,
        max_predictions=max(0, int(args.max_predictions)),
        dry_run=bool(args.dry_run),
        force=bool(args.force),
        continue_on_error=bool(args.continue_on_error),
    )
    transition_options = TransitionOptions(
        page_key_mode=str(args.page_key_mode),
        force=bool(args.force),
        sample_limit=int(args.sample),
        emit_catalog=bool(args.emit_transition_catalog),
        emit_gates=bool(args.emit_transition_gates),
        dry_run=bool(args.dry_run),
    )
    if "transition" in stages and "final-state" not in stages:
        raise SystemExit("The transition stage requires final-state in the same workflow run.")
    output_dir = resolve_output_dir(args)
    runs = discover_selected_runs(args)
    artifacts = WorkflowArtifactPolicy(output_dir)
    artifacts.ensure_base_dirs()
    write_workflow_plan(
        output_dir,
        runs,
        stages,
        final_state_options,
        plan_path=artifacts.matrix_plan_path,
    )
    context = WorkflowRunContext(
        runs=runs,
        output_dir=output_dir,
        stages=stages,
        prediction_scope=prediction_scope,
        final_state_options=final_state_options,
        transition_options=transition_options,
        artifacts=artifacts,
        reporter=WorkflowProgressReporter(),
        show_workload_sequence=bool(args.show_workload_sequence),
    )

    summaries: dict[str, Any] = {}
    quality_report: dict[str, Any] | None = None
    if "quality" in stages or "final-state" in stages or "transition" in stages:
        quality_report = QualityStageRunner().run(context, summaries)
        summaries["quality"] = quality_report
    workflow_mode = workflow_mode_from_quality(quality_report or {})
    if "final-state" in stages and "cross" in prediction_scope and workflow_mode != "forced_token_replay":
        raise SystemExit(
            f"Cross-config prediction requires the forced replay suite; current workflow mode is {workflow_mode}."
        )

    final_rows: list[dict[str, Any]] = []
    if "final-state" in stages:
        final_rows = FinalStateStageRunner().run(context, summaries)

    if "transition" in stages:
        TransitionStageRunner().run(context, summaries)

    write_workflow_summary(output_dir, runs, stages, prediction_scope, summaries, final_rows)
    return 0


def parse_csv_set(
    raw: str,
    choices: set[str],
    *,
    default: tuple[str, ...],
    label: str,
) -> set[str]:
    """解析逗号分隔集合，并校验允许值。"""

    values = {item.strip() for item in raw.split(",") if item.strip()}
    values = values or set(default)
    unknown = values - choices
    if unknown:
        raise SystemExit(f"Unknown {label}: {', '.join(sorted(unknown))}")
    return values


def parse_selector_values(*groups: list[str]) -> set[str]:
    """解析重复参数和逗号参数。"""

    result: set[str] = set()
    for group in groups:
        for raw in group:
            for item in str(raw).split(","):
                item = item.strip()
                if item:
                    result.add(item)
    return result


def resolve_repo_path(path: Path) -> Path:
    """把 CLI path 解析到 repo 绝对路径。"""

    path = path.expanduser()
    if path.is_absolute():
        return path
    return ROOT_DIR / path


def resolve_output_dir(args: argparse.Namespace) -> Path:
    """确定 workflow 输出目录。"""

    if args.output_dir:
        return resolve_repo_path(args.output_dir)
    if args.profile_run_dir:
        return resolve_repo_path(args.profile_run_dir[0]) / "modeling" / "hicache_state_workflow"
    return ROOT_DIR / "data/modeling_runs/hicache_state_workflow"


def discover_selected_runs(args: argparse.Namespace) -> list[Any]:
    """发现并过滤 profile runs。"""

    runs = discover_profile_runs(
        [resolve_repo_path(path) for path in args.profile_run_dir],
        [resolve_repo_path(path) for path in args.manifest],
    )
    runs = filter_runs(
        runs,
        input_ids=parse_selector_values(args.input, args.inputs),
        source_config_ids=parse_selector_values(args.source_config, args.source_configs),
        target_config_ids=parse_selector_values(args.target_config, args.target_configs),
    )
    if not runs:
        raise SystemExit("No profile manifests matched the requested workflow.")
    return runs


if __name__ == "__main__":
    raise SystemExit(main())
