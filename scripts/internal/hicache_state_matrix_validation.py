#!/usr/bin/env python3
"""执行 HiCache final-state matrix validation 的阶段一到三。"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
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


ROOT_DIR = Path(__file__).resolve().parents[2]
DEFAULT_STAGES = ("quality", "self", "cross")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """解析矩阵 validation CLI 参数。"""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--profile-run-dir",
        type=Path,
        action="append",
        default=[],
        help="包含多个 */profile_manifest.json 的 profiling run 目录，可重复。",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        action="append",
        default=[],
        help="显式加入一个 profile_manifest.json，可重复；S1A/B 一对一是矩阵特例。",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="输出目录；默认写到第一个 profile-run-dir/modeling/hicache_state_matrix_validation。",
    )
    parser.add_argument(
        "--stages",
        default=",".join(DEFAULT_STAGES),
        help="逗号分隔阶段：quality,self,cross。",
    )
    parser.add_argument("--input", action="append", default=[], help="只运行指定 input_id，可重复。")
    parser.add_argument("--source-config", action="append", default=[], help="只运行指定 source config_id，可重复。")
    parser.add_argument("--target-config", action="append", default=[], help="只运行指定 target config_id，可重复。")
    parser.add_argument("--exclude-self-in-cross", action="store_true", help="阶段三只运行非对角 cross prediction。")
    parser.add_argument("--force", action="store_true", help="即使 validation.json 已存在也重新运行 prediction。")
    parser.add_argument("--dry-run", action="store_true", help="只生成计划和 target config，不执行 scripts/model.sh。")
    parser.add_argument("--max-predictions", type=int, default=0, help="调试用：最多执行/计划 N 个 prediction，0 表示不限制。")
    parser.add_argument("--fail-fast", action="store_true", help="兼容旧命令；执行错误现在默认立即退出。")
    parser.add_argument("--continue-on-error", action="store_true", help="遇到 prediction 执行错误时继续跑后续格子。")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """CLI 入口。"""

    args = parse_args(argv)
    stages = parse_stages(args.stages)
    output_dir = resolve_output_dir(args)
    source_filters = set(args.source_config)
    target_filters = set(args.target_config)
    input_filters = set(args.input)
    runs = discover_profile_runs(
        [resolve_repo_path(path) for path in args.profile_run_dir],
        [resolve_repo_path(path) for path in args.manifest],
    )
    runs = filter_runs(
        runs,
        input_ids=input_filters,
        source_config_ids=source_filters,
        target_config_ids=target_filters,
    )
    if not runs:
        raise SystemExit("No profile manifests matched the requested matrix.")

    write_matrix_plan(output_dir, runs, args, stages)
    if "quality" in stages:
        report = build_quality_report(runs, output_dir)
        print_stage_summary("quality", report)

    all_specs = build_prediction_specs(
        runs,
        source_config_ids=source_filters,
        target_config_ids=target_filters,
        include_self=True,
    )
    if args.max_predictions > 0:
        all_specs = all_specs[: args.max_predictions]

    prediction_rows: list[dict[str, Any]] = []
    if "self" in stages or "cross" in stages:
        workload_ready_by_input = load_workload_ready_by_input(output_dir)
        selected_specs = select_prediction_specs(all_specs, stages, include_self_in_cross=not args.exclude_self_in_cross)
        for index, spec in enumerate(selected_specs, start=1):
            row = run_or_summarize_prediction(spec, output_dir, args, index, len(selected_specs), workload_ready_by_input)
            prediction_rows.append(row)
            if row.get("return_code", 0) != 0 and not args.continue_on_error:
                raise SystemExit(f"Prediction failed: {row.get('label')}; see {row.get('log_path')}")

    if "self" in stages:
        self_rows = [row for row in collect_prediction_rows(all_specs, output_dir) if row.get("is_self")]
        self_summary = matrix_summary(
            self_rows,
            schema="trace_sim.hicache.state_matrix.final_state_self.v1",
            stage="self",
        )
        write_json(output_dir / "final_state_self_5x4.json", self_summary)
        print_stage_summary("self", self_summary)

    if "cross" in stages:
        cross_specs = build_prediction_specs(
            runs,
            source_config_ids=source_filters,
            target_config_ids=target_filters,
            include_self=not args.exclude_self_in_cross,
        )
        if args.max_predictions > 0:
            cross_specs = cross_specs[: args.max_predictions]
        cross_rows = collect_prediction_rows(cross_specs, output_dir)
        cross_summary = matrix_summary(
            cross_rows,
            schema="trace_sim.hicache.state_matrix.final_state_cross.v1",
            stage="cross",
        )
        write_json(output_dir / "final_state_cross_5x4.json", cross_summary)
        print_stage_summary("cross", cross_summary)

    return 0


def parse_stages(raw: str) -> set[str]:
    """解析阶段集合。"""

    stages = {item.strip() for item in raw.split(",") if item.strip()}
    unknown = stages - set(DEFAULT_STAGES)
    if unknown:
        raise SystemExit(f"Unknown stages: {', '.join(sorted(unknown))}")
    return stages or set(DEFAULT_STAGES)


def resolve_repo_path(path: Path) -> Path:
    """把 CLI path 解析到 repo 绝对路径。"""

    path = path.expanduser()
    if path.is_absolute():
        return path
    return ROOT_DIR / path


def resolve_output_dir(args: argparse.Namespace) -> Path:
    """确定矩阵 validation 输出目录。"""

    if args.output_dir:
        return resolve_repo_path(args.output_dir)
    if args.profile_run_dir:
        return resolve_repo_path(args.profile_run_dir[0]) / "modeling" / "hicache_state_matrix_validation"
    return ROOT_DIR / "data/modeling_runs/hicache_state_matrix_validation"


def write_matrix_plan(output_dir: Path, runs: list[Any], args: argparse.Namespace, stages: set[str]) -> None:
    """写出本次矩阵执行计划。"""

    grouped = group_runs_by_input(runs)
    all_specs = build_prediction_specs(
        runs,
        source_config_ids=set(args.source_config),
        target_config_ids=set(args.target_config),
        include_self=True,
    )
    plan = {
        "schema": "trace_sim.hicache.state_matrix.plan.v1",
        "stages": sorted(stages),
        "dry_run": bool(args.dry_run),
        "force": bool(args.force),
        "run_count": len(runs),
        "config_ids": sorted({run.config_id for run in runs}),
        "input_ids": sorted(grouped),
        "inputs": {
            input_id: {
                "config_ids": sorted(by_config),
                "run_ids": {config_id: run.run_id for config_id, run in sorted(by_config.items())},
            }
            for input_id, by_config in grouped.items()
        },
        "prediction_count_full_same_input_matrix": len(all_specs),
        "self_prediction_count": sum(1 for spec in all_specs if spec.is_self),
        "cross_non_self_prediction_count": sum(1 for spec in all_specs if not spec.is_self),
        "runs": [
            {
                "run_id": run.run_id,
                "config_id": run.config_id,
                "input_id": run.input_id,
                "manifest_path": str(run.manifest_path),
                "hicache_config": run.hicache_config,
                "python_probe_file_count": len(run.python_probe_files),
            }
            for run in runs
        ],
    }
    write_json(output_dir / "matrix_plan.json", plan)


def select_prediction_specs(specs: list[PredictionSpec], stages: set[str], *, include_self_in_cross: bool) -> list[PredictionSpec]:
    """根据阶段选择需要执行的 prediction，避免重复执行对角线。"""

    selected: dict[tuple[str, str, str], PredictionSpec] = {}
    if "self" in stages:
        for spec in specs:
            if spec.is_self:
                selected[(spec.input_id, spec.source.config_id, spec.target.config_id)] = spec
    if "cross" in stages:
        for spec in specs:
            if include_self_in_cross or not spec.is_self:
                selected[(spec.input_id, spec.source.config_id, spec.target.config_id)] = spec
    return [selected[key] for key in sorted(selected)]


def run_or_summarize_prediction(
    spec: PredictionSpec,
    output_dir: Path,
    args: argparse.Namespace,
    index: int,
    total: int,
    workload_ready_by_input: dict[str, bool],
) -> dict[str, Any]:
    """执行或复用一个 prediction，并返回矩阵行摘要。"""

    pred_dir = prediction_output_dir(output_dir, spec)
    validation_path = pred_dir / "validation.json"
    config_path = write_target_model_config(spec.target, output_dir / "configs")
    command = build_model_command(spec, pred_dir, config_path)
    write_json(pred_dir / "command.json", {"command": command, "label": spec.label})

    skip_reason = skip_prediction_reason(spec, workload_ready_by_input)
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
    return row


def load_workload_ready_by_input(output_dir: Path) -> dict[str, bool]:
    """读取阶段一同 input workload 签名是否可用于 cross validation。"""

    report_path = output_dir / "profile_quality_5x4.json"
    if not report_path.is_file():
        return {}
    report = json.loads(report_path.read_text(encoding="utf-8"))
    signatures = report.get("input_workload_signatures") if isinstance(report.get("input_workload_signatures"), dict) else {}
    result: dict[str, bool] = {}
    for input_id, row in signatures.items():
        if isinstance(row, dict):
            result[str(input_id)] = bool(row.get("signature_match"))
    return result


def skip_prediction_reason(spec: PredictionSpec, workload_ready_by_input: dict[str, bool]) -> str:
    """返回 prediction 需要跳过的原因，空字符串表示可执行。"""

    if spec.is_self:
        return ""
    ready = workload_ready_by_input.get(spec.input_id)
    if ready is False:
        return "workload_signature_mismatch"
    return ""


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
            "invariant_coverage_ready": None,
            "missing_invariant_facts": [],
            "non_invariant_fact_usage": [],
            "sets_diff_by_tier": {},
        },
        "tier_count_deltas": {},
        "final_state_match": None,
        "non_invariant_fact_usage": [],
        "missing_invariant_facts": [],
    }


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
        row["non_invariant_fact_usage"] = hicache.get("non_invariant_fact_usage", [])
        row["missing_invariant_facts"] = hicache.get("missing_invariant_facts", [])
    return row


def collect_prediction_rows(specs: list[PredictionSpec], output_dir: Path) -> list[dict[str, Any]]:
    """从已有 prediction 输出目录收集矩阵行。"""

    rows: list[dict[str, Any]] = []
    for spec in specs:
        row_path = prediction_output_dir(output_dir, spec) / "matrix_row.json"
        if row_path.is_file():
            rows.append(json.loads(row_path.read_text(encoding="utf-8")))
            continue
        rows.append(build_prediction_row(spec, output_dir, return_code=0, elapsed_sec=0.0, command=[]))
    return rows


def read_log_tail(path: Path, limit: int = 8000) -> str:
    """读取日志尾部，避免把完整 Docker 输出塞进 summary。"""

    if not path.is_file():
        return ""
    text = path.read_text(encoding="utf-8", errors="replace")
    return text[-limit:]


def print_stage_summary(stage: str, report: dict[str, Any]) -> None:
    """向终端输出短摘要。"""

    if stage == "quality":
        print(
            json.dumps(
                {
                    "stage": stage,
                    "run_count": report.get("run_count"),
                    "quality_ready": report.get("quality_ready"),
                    "state_quality_ready_count": report.get("state_quality_ready_count"),
                    "profile_quality_ready_count": report.get("profile_quality_ready_count"),
                },
                ensure_ascii=False,
            ),
            flush=True,
        )
        return
    print(
        json.dumps(
            {
                "stage": stage,
                "prediction_count": report.get("prediction_count"),
                "validation_ready_count": report.get("validation_ready_count"),
                "final_state_match_count": report.get("final_state_match_count"),
                "final_state_pass_rate": report.get("final_state_pass_rate"),
            },
            ensure_ascii=False,
        ),
        flush=True,
    )


if __name__ == "__main__":
    raise SystemExit(main())
