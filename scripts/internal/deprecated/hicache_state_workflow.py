#!/usr/bin/env python3
"""HiCache profiling 后 validation 主入口。

该入口负责把 profile quality、final-state prediction 和 transition exactness
串成一个清晰 workflow。它只编排已有 profile 产物和 modeling 命令，不启动真实
profiling，也不修改 Python probe trace。
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parents[1]
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from hicache_state_matrix import (  # noqa: E402
    ACTIVE_STATE_TIERS,
    PredictionSpec,
    build_prediction_specs,
    build_quality_report,
    discover_profile_runs,
    filter_runs,
    group_runs_by_input,
    matrix_summary,
    prediction_output_dir,
    safe_slug,
    summarize_prediction,
    tier_count_delta,
    write_json,
    write_target_model_config,
)
from hicache_transition_exactness import compare_transition_matrix  # noqa: E402


DEFAULT_STAGES = ("quality", "final-state")
DEFAULT_PREDICTION_SCOPE = ("self", "cross")
STAGE_CHOICES = {"quality", "final-state", "transition"}
PREDICTION_SCOPE_CHOICES = {"self", "cross"}


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """解析 workflow CLI 参数。"""

    parser = argparse.ArgumentParser(
        description="Run HiCache post-profile validation workflow."
    )
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
    parser.add_argument("--max-predictions", type=int, default=0, help="Maximum predictions to execute; 0 means no limit.")
    parser.add_argument("--page-key-mode", default="strip_scope", choices=("strip_scope", "raw"))
    parser.add_argument("--sample", type=int, default=20, help="Maximum mismatch / evidence sample size.")
    parser.add_argument("--emit-transition-catalog", action="store_true", help="Emit transition mismatch catalog artifacts.")
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
    if "transition" in stages and "final-state" not in stages:
        raise SystemExit("The transition stage requires final-state in the same workflow run.")
    output_dir = resolve_output_dir(args)
    runs = discover_selected_runs(args)
    write_workflow_plan(output_dir, runs, args, stages, prediction_scope)

    summaries: dict[str, Any] = {}
    quality_report: dict[str, Any] | None = None
    if "quality" in stages or "final-state" in stages or "transition" in stages:
        if "quality" in stages:
            print_stage_start("quality", quality_stage_detail(runs))
        quality_report = build_quality_report(runs, output_dir, progress="quality" in stages)
        summaries["quality"] = quality_report
        if "quality" in stages:
            print_summary("quality", quality_report)
    workflow_mode = workflow_mode_from_quality(quality_report or {})
    if "final-state" in stages and "cross" in prediction_scope and workflow_mode != "forced_token_replay":
        raise SystemExit(
            "Cross-config prediction requires the forced replay suite; "
            f"current workflow mode is {workflow_mode}."
        )

    final_rows: list[dict[str, Any]] = []
    if "final-state" in stages:
        final_rows = run_final_state_predictions(
            runs,
            output_dir,
            args,
            prediction_scope,
            quality_report or {},
        )
        print_final_state_rows_summary(final_rows)
        if "self" in prediction_scope:
            self_summary = summarize_existing_predictions(
                runs,
                output_dir,
                args,
                scope={"self"},
                schema="trace_sim.hicache.state_workflow.final_state_self.v1",
                stage="final-state:self",
            )
            write_json(output_dir / "final_state_self.json", self_summary)
            summaries["final_state_self"] = self_summary
            print_summary("final-state:self", self_summary)
        if "cross" in prediction_scope:
            cross_summary = summarize_existing_predictions(
                runs,
                output_dir,
                args,
                scope={"cross"},
                schema="trace_sim.hicache.state_workflow.final_state_cross.v1",
                stage="final-state:cross",
            )
            write_json(output_dir / "final_state_cross.json", cross_summary)
            summaries["final_state_cross"] = cross_summary
            print_summary("final-state:cross", cross_summary)

    if "transition" in stages:
        transition_summary = run_transition_stage(output_dir, args)
        summaries["transition"] = transition_summary
        print_summary("transition", transition_summary)

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


def write_workflow_plan(
    output_dir: Path,
    runs: list[Any],
    args: argparse.Namespace,
    stages: set[str],
    prediction_scope: set[str],
) -> None:
    """写出 workflow 执行计划，同时保持 transition compare 需要的 matrix_plan 结构。"""

    grouped = group_runs_by_input(runs)
    source_filters = parse_selector_values(args.source_config, args.source_configs)
    target_filters = parse_selector_values(args.target_config, args.target_configs)
    all_specs = build_prediction_specs(
        runs,
        source_config_ids=source_filters,
        target_config_ids=target_filters,
        include_self=True,
    )
    selected_specs = select_specs_by_scope(all_specs, prediction_scope)
    if args.max_predictions > 0:
        selected_specs = selected_specs[: args.max_predictions]
    plan = {
        "schema": "trace_sim.hicache.state_workflow.plan.v1",
        "stages": sorted(stages),
        "prediction_scope": sorted(prediction_scope),
        "dry_run": bool(args.dry_run),
        "force": bool(args.force),
        "run_count": len(runs),
        "config_ids": sorted({run.config_id for run in runs}),
        "input_ids": sorted(grouped),
        "prediction_count": len(selected_specs),
        "self_prediction_count": sum(1 for spec in selected_specs if spec.is_self),
        "cross_prediction_count": sum(1 for spec in selected_specs if not spec.is_self),
        "inputs": {
            input_id: {
                "config_ids": sorted(by_config),
                "run_ids": {config_id: run.run_id for config_id, run in sorted(by_config.items())},
            }
            for input_id, by_config in grouped.items()
        },
        "runs": [
            {
                "run_id": run.run_id,
                "config_id": run.config_id,
                "input_id": run.input_id,
                "input_class": run.input_class,
                "manifest_path": str(run.manifest_path),
                "hicache_config": run.hicache_config,
                "python_probe_files": [str(path) for path in run.python_probe_files],
                "python_probe_file_count": len(run.python_probe_files),
            }
            for run in runs
        ],
    }
    write_json(output_dir / "matrix_plan.json", plan)


def select_specs_by_scope(specs: list[PredictionSpec], prediction_scope: set[str]) -> list[PredictionSpec]:
    """根据 self/cross scope 选择 prediction。"""

    selected: dict[tuple[str, str, str], PredictionSpec] = {}
    if "self" in prediction_scope:
        for spec in specs:
            if spec.is_self:
                selected[(spec.input_id, spec.source.config_id, spec.target.config_id)] = spec
    if "cross" in prediction_scope:
        for spec in specs:
            if not spec.is_self:
                selected[(spec.input_id, spec.source.config_id, spec.target.config_id)] = spec
    return [selected[key] for key in sorted(selected)]


def run_final_state_predictions(
    runs: list[Any],
    output_dir: Path,
    args: argparse.Namespace,
    prediction_scope: set[str],
    quality_report: dict[str, Any],
) -> list[dict[str, Any]]:
    """执行 final-state prediction，并写出每个格子的 matrix_row。"""

    specs = build_prediction_specs(
        runs,
        source_config_ids=parse_selector_values(args.source_config, args.source_configs),
        target_config_ids=parse_selector_values(args.target_config, args.target_configs),
        include_self=True,
    )
    specs = select_specs_by_scope(specs, prediction_scope)
    if args.max_predictions > 0:
        specs = specs[: args.max_predictions]

    print_stage_start("final-state", final_state_stage_detail(specs, dry_run=bool(args.dry_run)))
    quality_gate = build_quality_gate(quality_report)
    rows: list[dict[str, Any]] = []
    for index, spec in enumerate(specs, start=1):
        row = run_or_summarize_prediction(
            spec,
            output_dir,
            args,
            index,
            len(specs),
            quality_gate,
        )
        rows.append(row)
        if row.get("return_code", 0) != 0 and not args.continue_on_error:
            raise SystemExit(f"Prediction failed: {row.get('label')}; see {row.get('log_path')}")
    return rows


def print_final_state_rows_summary(rows: list[dict[str, Any]]) -> None:
    """打印 final-state 整体阶段摘要。"""

    prediction_count = len(rows)
    validation_ready_count = sum(1 for row in rows if row.get("validation_ready") is True)
    final_state_match_count = sum(1 for row in rows if row.get("final_state_match") is True)
    error_count = sum(1 for row in rows if row.get("return_code", 0) != 0)
    status = final_state_status_from_counts(
        prediction_count,
        validation_ready_count,
        final_state_match_count,
        error_count=error_count,
    )
    print(
        f"[finished final-state] {status} "
        f"predictions={prediction_count} "
        f"validation_ready={count_text(validation_ready_count, prediction_count)} "
        f"final_state_match={count_text(final_state_match_count, prediction_count)} "
        f"errors={error_count}",
        flush=True,
    )


def build_quality_gate(report: dict[str, Any]) -> dict[str, Any]:
    """把 quality report 规整成 prediction gate 索引。"""

    runs = report.get("runs") if isinstance(report.get("runs"), list) else []
    inputs = report.get("input_workload_signatures")
    return {
        "runs": {
            str(row.get("run_id")): row
            for row in runs
            if isinstance(row, dict) and row.get("run_id")
        },
        "inputs": inputs if isinstance(inputs, dict) else {},
    }


def skip_prediction_reason(spec: PredictionSpec, quality_gate: dict[str, Any]) -> str:
    """根据 profile quality 判断 prediction 是否应跳过。"""

    rows_by_run = quality_gate.get("runs") if isinstance(quality_gate.get("runs"), dict) else {}
    source_quality = rows_by_run.get(spec.source.run_id)
    target_quality = rows_by_run.get(spec.target.run_id)
    if isinstance(source_quality, dict) and not source_quality.get("state_quality_ready"):
        return "source_state_quality_not_ready"
    if isinstance(target_quality, dict) and not target_quality.get("state_quality_ready"):
        return "target_state_quality_not_ready"
    if spec.is_self:
        return ""

    inputs = quality_gate.get("inputs") if isinstance(quality_gate.get("inputs"), dict) else {}
    input_quality = inputs.get(spec.input_id)
    if not isinstance(input_quality, dict):
        return "input_quality_missing"
    if input_quality.get("signature_match") is not True:
        return "workload_signature_mismatch"
    if input_quality.get("forced_token_plan_signature_match") is not True:
        return "forced_token_plan_signature_mismatch"
    if input_quality.get("forced_token_bundle_signature_match") is not True:
        return "forced_token_bundle_signature_mismatch"
    return ""


def run_or_summarize_prediction(
    spec: PredictionSpec,
    output_dir: Path,
    args: argparse.Namespace,
    index: int,
    total: int,
    quality_gate: dict[str, Any],
) -> dict[str, Any]:
    """执行或复用一个 prediction，并返回矩阵行摘要。"""

    pred_dir = prediction_output_dir(output_dir, spec)
    validation_path = pred_dir / "validation.json"
    config_path = write_target_model_config(spec.target, output_dir / "configs")
    command = build_model_command(spec, pred_dir, config_path)
    write_json(pred_dir / "command.json", {"command": command, "label": spec.label})

    skip_reason = skip_prediction_reason(spec, quality_gate)
    if skip_reason:
        print(f"[{index}/{total}] skip {spec.label}: {skip_reason}", flush=True)
        row = build_skipped_prediction_row(spec, output_dir, command, skip_reason)
        write_json(pred_dir / "matrix_row.json", row)
        return row

    should_run = not args.dry_run and (args.force or not validation_path.is_file())
    if should_run:
        print(f"[{index}/{total}] run {spec.label}", flush=True)
        return_code, elapsed_sec = execute_command(command, pred_dir / "model.log")
    else:
        print(f"[{index}/{total}] skip {spec.label}", flush=True)
        return_code, elapsed_sec = (0, 0.0)

    row = build_prediction_row(spec, output_dir, return_code, elapsed_sec, command)
    write_json(pred_dir / "matrix_row.json", row)
    if should_run:
        print_prediction_result(index, total, row)
    return row


def print_prediction_result(index: int, total: int, row: dict[str, Any]) -> None:
    """打印 final-state prediction 的简短结果行。"""

    return_code = row.get("return_code")
    validation_ready = row.get("validation_ready")
    final_state_match = row.get("final_state_match")
    if return_code != 0:
        status = "error"
    elif validation_ready is not True:
        status = "not_ready"
    elif final_state_match is True:
        status = "ok"
    elif final_state_match is False:
        status = "mismatch"
    else:
        status = "unknown"
    elapsed_sec = row.get("elapsed_sec")
    elapsed_text = f" elapsed_sec={float(elapsed_sec):.2f}" if isinstance(elapsed_sec, (int, float)) else ""
    print(
        f"[{index}/{total}] result {status} "
        f"return_code={return_code} validation_ready={progress_value(validation_ready)} "
        f"final_state_match={progress_value(final_state_match)}{elapsed_text}",
        flush=True,
    )


def progress_value(value: Any) -> str:
    """把进度行中的 Python 值转成短字符串。"""

    if isinstance(value, bool) or value is None:
        return json.dumps(value)
    return str(value)


def build_model_command(spec: PredictionSpec, pred_dir: Path, config_path: Path) -> list[str]:
    """构造 scripts/model.sh 命令。"""

    command = [
        str(ROOT_DIR / "scripts/model.sh"),
        "--config",
        str(config_path),
        "--profile-manifest",
        str(spec.source.manifest_path),
        "--output-dir",
        str(pred_dir),
        "--mode",
        "cache_state",
        "--emit-module-summary",
        "--emit-validation",
    ]
    for oracle_path in spec.target.python_probe_files:
        command.extend(["--hicache-oracle-trace", str(oracle_path)])
    return command


def execute_command(command: list[str], log_path: Path) -> tuple[int, float]:
    """执行 modeling 命令，并把 stdout/stderr 写入日志。"""

    log_path.parent.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    with log_path.open("w", encoding="utf-8") as log_file:
        completed = subprocess.run(
            command,
            cwd=ROOT_DIR,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
    return completed.returncode, time.monotonic() - start


def build_prediction_row(
    spec: PredictionSpec,
    output_dir: Path,
    return_code: int,
    elapsed_sec: float,
    command: list[str],
) -> dict[str, Any]:
    """构造单个 prediction 的 summary row。"""

    pred_dir = prediction_output_dir(output_dir, spec)
    validation_summary = summarize_prediction(pred_dir / "validation.json")
    hicache = validation_summary.get("hicache_state", {})
    row = {
        "label": spec.label,
        "input_id": spec.input_id,
        "source_config_id": spec.source.config_id,
        "target_config_id": spec.target.config_id,
        "source_run_id": spec.source.run_id,
        "target_run_id": spec.target.run_id,
        "is_self": spec.is_self,
        "output_dir": str(pred_dir),
        "validation_path": str(pred_dir / "validation.json"),
        "log_path": str(pred_dir / "model.log"),
        "return_code": return_code,
        "elapsed_sec": elapsed_sec,
        "command": command,
        **validation_summary,
    }
    row["tier_count_deltas"] = {
        tier: tier_count_delta(row, tier)
        for tier in ACTIVE_STATE_TIERS
        if tier_count_delta(row, tier) is not None
    }
    if return_code != 0:
        row["execution_error_tail"] = read_log_tail(pred_dir / "model.log")
        if not row.get("validation_errors"):
            row["validation_errors"] = ["model_command_failed"]
        elif "model_command_failed" not in row["validation_errors"]:
            row["validation_errors"] = ["model_command_failed", *row["validation_errors"]]
    if hicache:
        row["final_state_match"] = hicache.get("final_state_match")
        row["state_model_fact_ready"] = hicache.get("state_model_fact_ready")
        row["missing_state_model_facts"] = hicache.get("missing_state_model_facts", [])
    return row


def build_skipped_prediction_row(
    spec: PredictionSpec,
    output_dir: Path,
    command: list[str],
    reason: str,
) -> dict[str, Any]:
    """构造未执行 prediction 的矩阵行。"""

    pred_dir = prediction_output_dir(output_dir, spec)
    return {
        "label": spec.label,
        "input_id": spec.input_id,
        "source_config_id": spec.source.config_id,
        "target_config_id": spec.target.config_id,
        "source_run_id": spec.source.run_id,
        "target_run_id": spec.target.run_id,
        "is_self": spec.is_self,
        "output_dir": str(pred_dir),
        "validation_path": str(pred_dir / "validation.json"),
        "log_path": str(pred_dir / "model.log"),
        "return_code": 0,
        "elapsed_sec": 0.0,
        "command": command,
        "skipped": True,
        "skip_reason": reason,
        "validation_ready": False,
        "validation_errors": [reason],
        "hicache_state": {
            "final_state_match": None,
            "state_model_fact_ready": None,
            "missing_state_model_facts": [],
            "sets_diff_by_tier": {},
        },
        "tier_count_deltas": {},
        "final_state_match": None,
        "state_model_fact_ready": None,
        "missing_state_model_facts": [],
    }


def summarize_existing_predictions(
    runs: list[Any],
    output_dir: Path,
    args: argparse.Namespace,
    *,
    scope: set[str],
    schema: str,
    stage: str,
) -> dict[str, Any]:
    """从已有 prediction row 汇总 self/cross 结果。"""

    specs = build_prediction_specs(
        runs,
        source_config_ids=parse_selector_values(args.source_config, args.source_configs),
        target_config_ids=parse_selector_values(args.target_config, args.target_configs),
        include_self=True,
    )
    specs = select_specs_by_scope(specs, scope)
    if args.max_predictions > 0:
        specs = specs[: args.max_predictions]
    rows = [collect_prediction_row(spec, output_dir) for spec in specs]
    return matrix_summary(rows, schema=schema, stage=stage)


def collect_prediction_row(spec: PredictionSpec, output_dir: Path) -> dict[str, Any]:
    """读取已有矩阵行；缺失时从 validation.json 尽力恢复。"""

    row_path = prediction_output_dir(output_dir, spec) / "matrix_row.json"
    if row_path.is_file():
        return json.loads(row_path.read_text(encoding="utf-8"))
    return build_prediction_row(spec, output_dir, return_code=0, elapsed_sec=0.0, command=[])


def run_transition_stage(output_dir: Path, args: argparse.Namespace) -> dict[str, Any]:
    """运行 transition exactness 矩阵比较。"""

    if args.dry_run:
        print_stage_start("transition", "dry run: no transition comparisons will be executed")
        summary = {
            "schema": "trace_sim.hicache.transition_exactness_matrix.v1",
            "matrix_dir": str(output_dir),
            "dry_run": True,
            "prediction_count": 0,
            "ready_count": 0,
            "exact_count": 0,
        }
        write_json(output_dir / "transition_exactness_matrix.json", summary)
        return summary

    summary = compare_transition_matrix(
        output_dir,
        page_key_mode=args.page_key_mode,
        force=bool(args.force),
        sample_limit=int(args.sample),
        emit_catalog=bool(args.emit_transition_catalog),
        emit_gates=bool(args.emit_transition_gates),
        catalog_output=None,
        gate_output=None,
        matrix_output_path=output_dir / "transition_exactness_matrix.json",
        progress=True,
    )
    write_json(output_dir / "transition_exactness_matrix.json", summary)
    return summary


def write_workflow_summary(
    output_dir: Path,
    runs: list[Any],
    stages: set[str],
    prediction_scope: set[str],
    summaries: dict[str, Any],
    final_rows: list[dict[str, Any]],
) -> None:
    """写出 workflow 顶层摘要。"""

    quality = summaries.get("quality") if isinstance(summaries.get("quality"), dict) else {}
    input_contracts = summarize_input_contracts(quality)
    forced_token_bundles = summarize_forced_token_bundles(quality)
    final_state_self = summarize_stage_for_workflow(
        summaries.get("final_state_self"),
        output_dir / "final_state_self.json",
    )
    final_state_cross = summarize_stage_for_workflow(
        summaries.get("final_state_cross"),
        output_dir / "final_state_cross.json",
    )
    transition = summarize_stage_for_workflow(
        summaries.get("transition"),
        output_dir / "transition_exactness_matrix.json",
    )
    payload = {
        "schema": "trace_sim.hicache.state_workflow.summary.v1",
        "stages": sorted(stages),
        "prediction_scope": sorted(prediction_scope),
        "workflow_mode": workflow_mode_from_quality(quality),
        "workflow_output_dir": str(output_dir),
        "profile_run_dirs": sorted({str(run.run_dir.parent) for run in runs}),
        "profile_run_dir": single_value_or_none(sorted({str(run.run_dir.parent) for run in runs})),
        "run_count": len(runs),
        "inputs": sorted({run.input_id for run in runs}),
        "input_ids": sorted({run.input_id for run in runs}),
        "config_ids": sorted({run.config_id for run in runs}),
        "final_state_prediction_rows": len(final_rows),
        "quality_ready": quality.get("quality_ready"),
        "input_contract_ready_count": input_contracts["ready_count"],
        "input_contract_count": input_contracts["input_count"],
        "quality": {
            "path": str(output_dir / "profile_quality.json") if quality else None,
            "ready": quality.get("quality_ready"),
            "state_quality_ready_count": quality.get("state_quality_ready_count"),
            "profile_quality_ready_count": quality.get("profile_quality_ready_count"),
            "run_count": quality.get("run_count"),
        },
        "input_contracts": input_contracts,
        "forced_token_bundles": forced_token_bundles,
        "final_state_self": final_state_self,
        "final_state_cross": final_state_cross,
        "transition": transition,
    }
    write_json(output_dir / "workflow_summary.json", payload)


def summarize_forced_token_bundles(quality: dict[str, Any]) -> dict[str, Any]:
    """汇总 replay runs 显式依赖的 capture bundle。"""

    rows = quality.get("runs") if isinstance(quality.get("runs"), list) else []
    bundles: dict[str, dict[str, Any]] = {}
    for row in rows:
        if not isinstance(row, dict):
            continue
        bundle_sha256 = row.get("forced_token_bundle_sha256")
        if not bundle_sha256:
            continue
        key = str(bundle_sha256)
        bundle = bundles.setdefault(
            key,
            {
                "sha256": key,
                "bundle_ids": set(),
                "paths": set(),
                "input_ids": set(),
                "run_ids": set(),
            },
        )
        if row.get("forced_token_bundle_id"):
            bundle["bundle_ids"].add(str(row["forced_token_bundle_id"]))
        if row.get("forced_token_bundle_path"):
            bundle["paths"].add(str(row["forced_token_bundle_path"]))
        if row.get("input_id"):
            bundle["input_ids"].add(str(row["input_id"]))
        if row.get("run_id"):
            bundle["run_ids"].add(str(row["run_id"]))
    serialized = [
        {
            "sha256": item["sha256"],
            "bundle_ids": sorted(item["bundle_ids"]),
            "paths": sorted(item["paths"]),
            "input_ids": sorted(item["input_ids"]),
            "run_count": len(item["run_ids"]),
        }
        for item in bundles.values()
    ]
    return {
        "bundle_count": len(serialized),
        "single_bundle": len(serialized) == 1,
        "bundles": sorted(serialized, key=lambda item: item["sha256"]),
    }


def workflow_mode_from_quality(quality: dict[str, Any]) -> str:
    """根据 workload forced-token 摘要推导 workflow 输入模式。"""

    rows = quality.get("runs") if isinstance(quality.get("runs"), list) else []
    modes = {
        str(row.get("forced_token_mode") or "none")
        for row in rows
        if isinstance(row, dict)
    }
    if not modes:
        return "unknown"
    if modes == {"replay"}:
        return "forced_token_replay"
    if modes == {"capture"}:
        return "forced_token_capture"
    if modes == {"none"}:
        return "normal_generate"
    return "mixed"


def summarize_input_contracts(quality: dict[str, Any]) -> dict[str, Any]:
    """把 quality report 的同 input 合同压缩成 workflow 顶层摘要。"""

    raw_inputs = quality.get("input_workload_signatures")
    inputs = raw_inputs if isinstance(raw_inputs, dict) else {}
    rows: dict[str, dict[str, Any]] = {}
    for input_id, item in sorted(inputs.items()):
        if not isinstance(item, dict):
            continue
        ready = item.get("input_contract_ready") is True
        rows[str(input_id)] = {
            "ready": ready,
            "run_count": item.get("run_count"),
            "config_ids": item.get("config_ids", []),
            "signature_match": item.get("signature_match"),
            "signature_count": item.get("signature_count"),
            "forced_token_plan_signature_match": item.get("forced_token_plan_signature_match"),
            "forced_token_plan_signature_count": item.get("forced_token_plan_signature_count"),
            "forced_token_plan_signatures": item.get("forced_token_plan_signatures", []),
            "forced_token_bundle_signature_match": item.get("forced_token_bundle_signature_match"),
            "forced_token_bundle_signature_count": item.get("forced_token_bundle_signature_count"),
            "forced_token_bundle_signatures": item.get("forced_token_bundle_signatures", []),
            "forced_token_bundle_ids": item.get("forced_token_bundle_ids", []),
        }
    return {
        "ready_count": sum(1 for item in rows.values() if item["ready"]),
        "input_count": len(rows),
        "all_ready": bool(rows) and all(item["ready"] for item in rows.values()),
        "inputs": rows,
    }


def summarize_stage_for_workflow(summary: Any, path: Path) -> dict[str, Any] | None:
    """抽取 stage 大 JSON 中最适合顶层展示的少数字段。"""

    if not isinstance(summary, dict):
        return None
    keys = (
        "stage",
        "prediction_count",
        "validation_ready_count",
        "ready_count",
        "final_state_match_count",
        "final_state_pass_rate",
        "exact_count",
        "transition_count_exact_count",
    )
    compact = {
        key: summary.get(key)
        for key in keys
        if key in summary
    }
    if "final_state_match_count" in compact:
        compact["pass_count"] = compact["final_state_match_count"]
    elif "exact_count" in compact:
        compact["pass_count"] = compact["exact_count"]
    compact["path"] = str(path)
    return compact


def single_value_or_none(values: list[str]) -> str | None:
    """只有一个值时返回该值，否则返回 None，避免误导多 profile-run-dir workflow。"""

    return values[0] if len(values) == 1 else None


def read_log_tail(path: Path, limit: int = 8000) -> str:
    """读取日志尾部，避免把完整 Docker 输出塞进 summary。"""

    if not path.is_file():
        return ""
    text = path.read_text(encoding="utf-8", errors="replace")
    return text[-limit:]


def print_summary(stage: str, report: dict[str, Any]) -> None:
    """向终端输出短摘要。"""

    if stage == "quality":
        input_contracts = summarize_input_contracts(report)
        run_count = report.get("run_count")
        status = "ok" if report.get("quality_ready") is True else "needs_attention"
        print(
            f"[finished {stage}] {status} "
            f"runs={value_text(run_count)} "
            f"state_ready={count_text(report.get('state_quality_ready_count'), run_count)} "
            f"profile_ready={count_text(report.get('profile_quality_ready_count'), run_count)} "
            f"input_contracts={count_text(input_contracts['ready_count'], input_contracts['input_count'])}",
            flush=True,
        )
        return
    elif stage == "transition":
        prediction_count = report.get("prediction_count")
        status = transition_stage_status(report)
        print(
            f"[finished {stage}] {status} "
            f"predictions={value_text(prediction_count)} "
            f"ready={count_text(report.get('ready_count'), prediction_count)} "
            f"exact={count_text(report.get('exact_count'), prediction_count)} "
            f"transition_count_exact={count_text(report.get('transition_count_exact_count'), prediction_count)}",
            flush=True,
        )
        return
    else:
        prediction_count = report.get("prediction_count")
        status = final_state_stage_status(report)
        prefix = "summary" if stage.startswith("final-state:") else "finished"
        print(
            f"[{prefix} {stage}] {status} "
            f"predictions={value_text(prediction_count)} "
            f"validation_ready={count_text(report.get('validation_ready_count'), prediction_count)} "
            f"final_state_match={count_text(report.get('final_state_match_count'), prediction_count)} "
            f"pass_rate={percent_text(report.get('final_state_pass_rate'))}",
            flush=True,
        )


def print_stage_start(stage: str, detail: str) -> None:
    """向终端输出一个人读的阶段开始行。"""

    print(f"[running {stage}] {detail}", flush=True)


def quality_stage_detail(runs: list[Any]) -> str:
    """返回 quality 阶段的人读说明。"""

    return (
        f"profile quality audit: runs={len(runs)} "
        f"inputs={len({run.input_id for run in runs})} "
        f"configs={len({run.config_id for run in runs})}"
    )


def final_state_stage_detail(specs: list[PredictionSpec], *, dry_run: bool) -> str:
    """返回 final-state 阶段的人读说明。"""

    self_count = sum(1 for spec in specs if spec.is_self)
    cross_count = len(specs) - self_count
    dry_run_text = " dry_run=true" if dry_run else ""
    return (
        f"state prediction matrix: predictions={len(specs)} "
        f"self={self_count} cross={cross_count}{dry_run_text}"
    )


def final_state_stage_status(report: dict[str, Any]) -> str:
    """把 final-state summary 规整成人读状态。"""

    prediction_count = report.get("prediction_count")
    validation_ready_count = report.get("validation_ready_count")
    final_state_match_count = report.get("final_state_match_count")
    if not all(
        is_plain_int(value)
        for value in (prediction_count, validation_ready_count, final_state_match_count)
    ):
        return "unknown"
    return final_state_status_from_counts(
        prediction_count,
        validation_ready_count,
        final_state_match_count,
        error_count=0,
    )


def final_state_status_from_counts(
    prediction_count: int,
    validation_ready_count: int,
    final_state_match_count: int,
    *,
    error_count: int,
) -> str:
    """根据 final-state 计数字段返回人读状态。"""

    if prediction_count == 0:
        return "empty"
    if error_count:
        return "error"
    if validation_ready_count != prediction_count:
        return "not_ready"
    if final_state_match_count == prediction_count:
        return "ok"
    return "mismatch"


def transition_stage_status(report: dict[str, Any]) -> str:
    """把 transition summary 规整成人读状态。"""

    prediction_count = report.get("prediction_count")
    ready_count = report.get("ready_count")
    exact_count = report.get("exact_count")
    if not all(is_plain_int(value) for value in (prediction_count, ready_count, exact_count)):
        return "unknown"
    return transition_status_from_counts(prediction_count, ready_count, exact_count)


def transition_status_from_counts(
    prediction_count: int,
    ready_count: int,
    exact_count: int,
) -> str:
    """根据 transition 计数字段返回人读状态。"""

    if prediction_count == 0:
        return "empty"
    if ready_count != prediction_count:
        return "not_ready"
    if exact_count == prediction_count:
        return "ok"
    return "mismatch"


def count_text(count: Any, total: Any) -> str:
    """把 count/total 转成人读短字符串。"""

    return f"{value_text(count)}/{value_text(total)}"


def is_plain_int(value: Any) -> bool:
    """判断摘要计数字段是否为普通整数。"""

    return isinstance(value, int) and not isinstance(value, bool)


def value_text(value: Any) -> str:
    """把摘要值转成人读短字符串。"""

    return "unknown" if value is None else str(value)


def percent_text(value: Any) -> str:
    """把 0-1 pass rate 转成人读百分比。"""

    if isinstance(value, (int, float)):
        return f"{value * 100:.1f}%"
    return "unknown"


if __name__ == "__main__":
    raise SystemExit(main())
