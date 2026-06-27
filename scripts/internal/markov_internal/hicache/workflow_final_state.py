"""Final-state prediction stage for HiCache validation workflows."""

from __future__ import annotations

import json
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..common.io import write_json
from ..common.paths import ROOT_DIR
from .matrix_discovery import build_prediction_specs
from .matrix_prediction import (
    ACTIVE_STATE_TIERS,
    matrix_summary,
    prediction_output_dir,
    summarize_prediction,
    tier_count_delta,
    write_target_model_config,
)
from .matrix_types import PredictionSpec
from .workflow_progress import (
    final_state_stage_detail,
    print_prediction_result,
    print_stage_start,
)


@dataclass(frozen=True)
class FinalStateOptions:
    """Final-state stage options independent from argparse."""

    source_config_ids: set[str]
    target_config_ids: set[str]
    prediction_scope: set[str]
    max_predictions: int = 0
    dry_run: bool = False
    force: bool = False
    continue_on_error: bool = False


def prediction_specs_for_options(
    runs: list[Any],
    options: FinalStateOptions,
    *,
    scope: set[str] | None = None,
) -> list[PredictionSpec]:
    """Build prediction specs for the selected configs and scope."""

    specs = build_prediction_specs(
        runs,
        source_config_ids=options.source_config_ids,
        target_config_ids=options.target_config_ids,
        include_self=True,
    )
    selected = select_specs_by_scope(specs, scope or options.prediction_scope)
    if options.max_predictions > 0:
        selected = selected[: options.max_predictions]
    return selected


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
    options: FinalStateOptions,
    quality_report: dict[str, Any],
) -> list[dict[str, Any]]:
    """执行 final-state prediction，并写出每个格子的 matrix_row。"""

    specs = prediction_specs_for_options(runs, options)
    print_stage_start("final-state", final_state_stage_detail(specs, dry_run=options.dry_run))
    quality_gate = build_quality_gate(quality_report)
    rows: list[dict[str, Any]] = []
    for index, spec in enumerate(specs, start=1):
        row = run_or_summarize_prediction(
            spec,
            output_dir,
            options,
            index,
            len(specs),
            quality_gate,
        )
        rows.append(row)
        if row.get("return_code", 0) != 0 and not options.continue_on_error:
            raise SystemExit(f"Prediction failed: {row.get('label')}; see {row.get('log_path')}")
    return rows


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
    options: FinalStateOptions,
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

    should_run = not options.dry_run and (options.force or not validation_path.is_file())
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
    options: FinalStateOptions,
    *,
    scope: set[str],
    schema: str,
    stage: str,
) -> dict[str, Any]:
    """从已有 prediction row 汇总 self/cross 结果。"""

    rows = [
        collect_prediction_row(spec, output_dir)
        for spec in prediction_specs_for_options(runs, options, scope=scope)
    ]
    return matrix_summary(rows, schema=schema, stage=stage)


def collect_prediction_row(spec: PredictionSpec, output_dir: Path) -> dict[str, Any]:
    """读取已有矩阵行；缺失时从 validation.json 尽力恢复。"""

    row_path = prediction_output_dir(output_dir, spec) / "matrix_row.json"
    if row_path.is_file():
        return json.loads(row_path.read_text(encoding="utf-8"))
    return build_prediction_row(spec, output_dir, return_code=0, elapsed_sec=0.0, command=[])


def read_log_tail(path: Path, limit: int = 8000) -> str:
    """读取日志尾部，避免把完整 Docker 输出塞进 summary。"""

    if not path.is_file():
        return ""
    text = path.read_text(encoding="utf-8", errors="replace")
    return text[-limit:]
