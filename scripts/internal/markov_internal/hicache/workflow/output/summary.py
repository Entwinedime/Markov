"""HiCache validation 的 workflow 级 summary payload 工具。"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ....common.io import write_json


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
        output_dir / "stages" / "final_state" / "self_summary.json",
    )
    final_state_cross = summarize_stage_for_workflow(
        summaries.get("final_state_cross"),
        output_dir / "stages" / "final_state" / "cross_summary.json",
    )
    transition = summarize_stage_for_workflow(
        summaries.get("transition"),
        output_dir / "stages" / "transition" / "summary.json",
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
        "input_ids": sorted({run.input_id for run in runs}),
        "config_ids": sorted({run.config_id for run in runs}),
        "final_state_prediction_rows": len(final_rows),
        "workflow_input_ready": quality.get("workflow_input_ready"),
        "input_contract_ready_count": input_contracts["ready_count"],
        "input_contract_count": input_contracts["input_count"],
        "quality": {
            "path": str(output_dir / "stages" / "quality" / "summary.json") if quality else None,
            "ready": quality.get("workflow_input_ready"),
            "workflow_input_ready_count": quality.get("workflow_input_ready_count"),
            "state_model_input_ready_count": quality.get("state_model_input_ready_count"),
            "strict_diagnostic_coverage_ready_count": quality.get("strict_diagnostic_coverage_ready_count"),
            "artifact_ready_count": quality.get("artifact_ready_count"),
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
                "bundle_ids": set(),
                "input_ids": set(),
                "run_ids": set(),
            },
        )
        if row.get("forced_token_bundle_id"):
            bundle["bundle_ids"].add(str(row["forced_token_bundle_id"]))
        if row.get("input_id"):
            bundle["input_ids"].add(str(row["input_id"]))
        if row.get("run_id"):
            bundle["run_ids"].add(str(row["run_id"]))
    serialized = [
        {
            "bundle_ids": sorted(item["bundle_ids"]),
            "input_ids": sorted(item["input_ids"]),
            "run_count": len(item["run_ids"]),
        }
        for item in bundles.values()
    ]
    return {
        "bundle_count": len(serialized),
        "single_bundle": len(serialized) == 1,
        "bundles": sorted(serialized, key=lambda item: (item["bundle_ids"], item["input_ids"])),
    }


def workflow_mode_from_quality(quality: dict[str, Any]) -> str:
    """根据 workload forced-token 摘要推导 workflow 输入模式。"""

    rows = quality.get("runs") if isinstance(quality.get("runs"), list) else []
    modes = {
        str(row.get("forced_token_mode")) for row in rows if isinstance(row, dict) and row.get("forced_token_mode")
    }
    if not modes:
        return "not_checked"
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
            "canonical_workload_ready": item.get("canonical_workload_ready"),
            "signature_match": item.get("signature_match"),
            "signature_count": item.get("signature_count"),
            "sequence_match": item.get("sequence_match"),
            "sequence_signature_count": item.get("sequence_signature_count"),
        }
        if "forced_token_plan_signature_match" in item:
            rows[str(input_id)].update(
                {
                    "forced_token_plan_signature_match": item.get("forced_token_plan_signature_match"),
                    "forced_token_plan_signature_count": item.get("forced_token_plan_signature_count"),
                    "forced_token_bundle_signature_match": item.get("forced_token_bundle_signature_match"),
                    "forced_token_bundle_signature_count": item.get("forced_token_bundle_signature_count"),
                    "forced_token_bundle_ids": item.get("forced_token_bundle_ids", []),
                }
            )
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
    compact = {key: summary.get(key) for key in keys if key in summary}
    if "final_state_match_count" in compact:
        compact["pass_count"] = compact["final_state_match_count"]
    elif "exact_count" in compact:
        compact["pass_count"] = compact["exact_count"]
    compact["path"] = str(path)
    return compact


def single_value_or_none(values: list[str]) -> str | None:
    """只有一个值时返回该值，否则返回 None，避免误导多 profile-run-dir workflow。"""

    return values[0] if len(values) == 1 else None
