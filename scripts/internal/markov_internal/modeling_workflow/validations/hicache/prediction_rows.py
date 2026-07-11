"""Construction and aggregation of HiCache cache-state prediction rows."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ....common.io import load_json
from ...types import ModelRunResult


ACTIVE_STATE_TIERS = (
    "l1_resident_pages",
    "l2_resident_pages",
    "dirty_pages",
    "backuped_pages",
    "evicted_pages",
    "locked_pages",
)


def build_prediction_row(result: ModelRunResult) -> dict[str, Any]:
    """Construct a compact final-state row from one cache-state model run."""

    spec = result.spec
    if spec.prediction is None:
        raise ValueError(f"missing prediction metadata for cache-state model run: {spec.run_id}")
    prediction = spec.prediction
    validation_summary = summarize_prediction(result.artifacts.validation_json)
    hicache = validation_summary.get("hicache_state", {})
    row = {
        "model_run_id": spec.run_id,
        "label": prediction.label,
        "input_id": prediction.input_id,
        "source_config_id": prediction.source.config_id,
        "target_config_id": prediction.target.config_id,
        "source_run_id": prediction.source.run_id,
        "target_run_id": prediction.target.run_id,
        "is_self": prediction.is_self,
        "output_dir": str(spec.output_dir),
        "validation_path": str(result.artifacts.validation_json),
        "log_path": str(result.artifacts.model_log),
        "return_code": result.return_code,
        "elapsed_sec": result.elapsed_sec,
        "skipped": result.skipped,
        "dry_run": result.dry_run,
        "skip_reason": result.skip_reason or None,
        **validation_summary,
    }
    tier_deltas: dict[str, int] = {}
    for tier in ACTIVE_STATE_TIERS:
        delta = tier_count_delta(row, tier)
        if delta is not None:
            tier_deltas[tier] = delta
    row["tier_count_deltas"] = tier_deltas
    if result.return_code != 0:
        row["execution_error_tail"] = result.execution_error_tail
        errors = row.get("validation_errors")
        if not isinstance(errors, list) or not errors:
            row["validation_errors"] = ["model_command_failed"]
        elif "model_command_failed" not in errors:
            row["validation_errors"] = ["model_command_failed", *errors]
    if result.skipped:
        reason = result.skip_reason or "skipped"
        row["validation_ready"] = False
        row["validation_errors"] = [reason]
    if isinstance(hicache, dict):
        row["final_state_match"] = hicache.get("final_state_match")
        row["state_model_fact_ready"] = hicache.get("state_model_fact_ready")
        row["missing_state_model_facts"] = hicache.get("missing_state_model_facts", [])
    return row


def summarize_final_state_rows(rows: list[dict[str, Any]], *, scope: str) -> dict[str, Any]:
    """Aggregate final-state rows for one prediction scope."""

    return prediction_summary(
        rows,
        schema=f"trace_sim.modeling_workflow.validation.hicache_final_state_{scope}.v1",
        stage=f"hicache_final_state:{scope}",
    )


def summarize_prediction(validation_path: Path) -> dict[str, Any]:
    """Extract the compact workflow-facing fields from ``validation.json``."""

    if not validation_path.is_file():
        return {
            "validation_ready": False,
            "validation_errors": ["missing_validation_json"],
            "hicache_state": {},
        }
    validation = load_json(validation_path)
    if not isinstance(validation, dict):
        return {
            "validation_ready": False,
            "validation_errors": ["invalid_validation_json"],
            "hicache_state": {},
        }
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


def prediction_summary(rows: list[dict[str, Any]], *, schema: str, stage: str) -> dict[str, Any]:
    """Aggregate final-state prediction rows without duplicating row details."""

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
        "note": (
            "Stage summary keeps only aggregate counters. Full per-prediction rows live under "
            "artifacts/validations/hicache_final_state/."
        ),
    }


def tier_count_delta(row: dict[str, Any], tier: str) -> int | None:
    """Return the model-minus-oracle count delta for one state tier."""

    hicache = row.get("hicache_state") if isinstance(row.get("hicache_state"), dict) else {}
    model_counts = hicache.get("normalized_model_final_state_counts")
    oracle_counts = hicache.get("normalized_oracle_final_state_counts")
    model = model_counts if isinstance(model_counts, dict) else {}
    oracle = oracle_counts if isinstance(oracle_counts, dict) else {}
    if tier not in model and tier not in oracle:
        return None
    return int(model.get(tier, 0) or 0) - int(oracle.get(tier, 0) or 0)
