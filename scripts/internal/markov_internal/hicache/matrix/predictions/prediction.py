"""HiCache state 矩阵的 prediction 产物与 summary 工具。"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ....common.io import load_json
from ..runs.types import PredictionSpec, safe_slug


ACTIVE_STATE_TIERS = (
    "l1_resident_pages",
    "l2_resident_pages",
    "dirty_pages",
    "backuped_pages",
    "evicted_pages",
    "locked_pages",
)


def prediction_output_dir(output_dir: Path, spec: PredictionSpec) -> Path:
    """返回一个 prediction 格子的输出目录。"""

    return (
        output_dir
        / "predictions"
        / safe_slug(spec.input_id)
        / f"{safe_slug(spec.source.config_id)}__to__{safe_slug(spec.target.config_id)}"
    )


def summarize_prediction(validation_path: Path) -> dict[str, Any]:
    """从 validation.json 中提取矩阵汇总字段。"""

    if not validation_path.is_file():
        return {
            "validation_ready": False,
            "validation_errors": ["missing_validation_json"],
            "hicache_state": {},
        }
    validation = load_json(validation_path)
    hicache = validation.get("hicache_state") if isinstance(validation.get("hicache_state"), dict) else {}
    return {
        "validation_ready": bool(validation.get("validation_ready")),
        "validation_errors": validation.get("validation_errors", []),
        "dag": validation.get("dag", {}),
        "e2e": validation.get("e2e", {}),
        "hicache_state": {
            "state_trace_ready": hicache.get("state_trace_ready"),
            "state_trace_events": hicache.get("state_trace_events"),
            "model_transition_events": hicache.get("model_transition_events"),
            "state_model_fact_ready": hicache.get("state_model_fact_ready"),
            "missing_state_model_facts": hicache.get("missing_state_model_facts", []),
            "missing_state_model_fact_counts": hicache.get("missing_state_model_fact_counts", {}),
            "final_state_match": hicache.get("final_state_match"),
            "raw_final_state_match": hicache.get("raw_final_state_match"),
            "normalized_model_final_state_counts": hicache.get("normalized_model_final_state_counts", {}),
            "normalized_oracle_final_state_counts": hicache.get("normalized_oracle_final_state_counts", {}),
            "sets_diff_by_tier": hicache.get("sets_diff_by_tier", {}),
            "first_mismatch": hicache.get("first_mismatch"),
            "capacity_config_audit": hicache.get("capacity_config_audit", {}),
            "predicted_state_trace_path": hicache.get("predicted_state_trace_path"),
        },
    }


def matrix_summary(rows: list[dict[str, Any]], *, schema: str, stage: str) -> dict[str, Any]:
    """汇总 self/cross prediction rows。"""

    pass_rows = [row for row in rows if row.get("hicache_state", {}).get("final_state_match") is True]
    ready_rows = [row for row in rows if row.get("validation_ready")]
    state_model_fact_ready_rows = [
        row for row in rows if row.get("hicache_state", {}).get("state_model_fact_ready") is True
    ]
    by_input: dict[str, dict[str, Any]] = {}
    for input_id in sorted({str(row.get("input_id")) for row in rows}):
        input_rows = [row for row in rows if row.get("input_id") == input_id]
        by_input[input_id] = {
            "prediction_count": len(input_rows),
            "final_state_match_count": sum(
                1 for row in input_rows if row.get("hicache_state", {}).get("final_state_match") is True
            ),
            "validation_ready_count": sum(1 for row in input_rows if row.get("validation_ready")),
        }
    return {
        "schema": schema,
        "stage": stage,
        "prediction_count": len(rows),
        "validation_ready_count": len(ready_rows),
        "state_model_fact_ready_count": len(state_model_fact_ready_rows),
        "final_state_match_count": len(pass_rows),
        "final_state_pass_rate": len(pass_rows) / len(rows) if rows else None,
        "by_input": by_input,
        "note": "Stage summary keeps only aggregate counters. Full per-prediction rows live in predictions/<input>/<source>__to__<target>/matrix_row.json.",
    }


def tier_count_delta(row: dict[str, Any], tier: str) -> int | None:
    """计算某个 tier 的 model/oracle count delta。"""

    hicache = row.get("hicache_state") if isinstance(row.get("hicache_state"), dict) else {}
    model_counts = hicache.get("normalized_model_final_state_counts")
    oracle_counts = hicache.get("normalized_oracle_final_state_counts")
    model = model_counts if isinstance(model_counts, dict) else {}
    oracle = oracle_counts if isinstance(oracle_counts, dict) else {}
    if tier not in model and tier not in oracle:
        return None
    return int(model.get(tier, 0) or 0) - int(oracle.get(tier, 0) or 0)
