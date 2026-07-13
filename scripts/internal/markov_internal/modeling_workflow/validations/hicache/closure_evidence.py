"""Shared final-DAG evidence used by HiCache diagnostic closure reports."""

from __future__ import annotations

from typing import Any


PREFETCH_READINESS_STATUS = "payload_only_control_pipeline_unmodeled"


def prediction_target_key(row: dict[str, Any]) -> tuple[str, str]:
    """Return the input/target identity shared by self and cross predictions."""

    return str(row.get("input_id") or ""), str(row.get("target_config_id") or "")


def final_dag_evidence(
    row: dict[str, Any],
    final_dag_rows: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    """Project one final-DAG row to the fields allowed in closure classification."""

    model_run_id = str(row.get("model_run_id") or "")
    final_row = final_dag_rows.get(model_run_id, {})
    return {
        "final_dag_row_present": bool(final_row),
        "final_dag_ready": final_row.get("ready"),
        "prefetch_readiness_status": final_row.get("prefetch_readiness_status"),
        "shape_acceptance_ready": final_row.get("shape_acceptance_ready"),
        "readiness_limitation_mismatch_count": int(final_row.get("shape_readiness_limitation_mismatch_count") or 0),
        "acceptance_mismatch_count": int(final_row.get("shape_acceptance_mismatch_count") or 0),
        "schedule_sensitive_mismatch_count": int(final_row.get("shape_schedule_sensitive_mismatch_count") or 0),
        "schedule_sensitive_count": int(final_row.get("schedule_sensitive_count") or 0),
        "alternate_evidence_ready": final_row.get("alternate_evidence_ready"),
        "state_model_fact_ready": final_row.get("state_model_fact_ready"),
        "final_state_match": final_row.get("final_state_match"),
    }


def accepted_shape(evidence: dict[str, Any]) -> bool:
    """Return whether all modeled, schedule-invariant shape rows are accepted."""

    return bool(
        evidence.get("final_dag_row_present") is True
        and evidence.get("final_dag_ready") is True
        and evidence.get("shape_acceptance_ready") is True
        and int(evidence.get("acceptance_mismatch_count") or 0) == 0
        and evidence.get("state_model_fact_ready") is True
    )


def readiness_only_shape(evidence: dict[str, Any]) -> bool:
    """Return whether the only accepted shape differences are readiness limitations."""

    return bool(
        accepted_shape(evidence)
        and evidence.get("prefetch_readiness_status") == PREFETCH_READINESS_STATUS
        and int(evidence.get("readiness_limitation_mismatch_count") or 0) > 0
    )
