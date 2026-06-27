"""Workflow-level summary payload helpers for HiCache validation."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ..common.io import write_json


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
