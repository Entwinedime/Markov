"""Diagnostic closure classification for HiCache final-state outcomes."""

from __future__ import annotations

import collections
from typing import Any

from .closure_evidence import (
    accepted_shape,
    final_dag_evidence,
    prediction_target_key,
    readiness_only_shape,
)


FINAL_STATE_EXACT = "final_state_exact"
PREFETCH_READINESS_LIMITATION = "payload_only_prefetch_readiness_limitation"
CROSS_SCHEDULE_SENSITIVE = "cross_arrival_schedule_sensitive"
UNRELATED_SEMANTIC_MISMATCH = "unrelated_semantic_mismatch"
NOT_READY = "not_ready"

_FINAL_STATE_MISMATCH = "hicache_final_state_mismatch"


def build_final_state_closure_report(
    rows: list[dict[str, Any]],
    final_dag_rows: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    """Explain raw final-state outcomes without changing exactness or readiness."""

    self_rows = {prediction_target_key(row): row for row in rows if row.get("is_self") is True}
    self_classifications: dict[tuple[str, str], str] = {}
    for key, row in self_rows.items():
        category, _ = _classify_self(row, final_dag_evidence(row, final_dag_rows))
        self_classifications[key] = category

    report_rows: list[dict[str, Any]] = []
    for row in rows:
        evidence = final_dag_evidence(row, final_dag_rows)
        target_key = prediction_target_key(row)
        target_self = self_rows.get(target_key)
        target_self_category = self_classifications.get(target_key)
        if row.get("is_self") is True:
            category, reason = _classify_self(row, evidence)
        else:
            category, reason = _classify_cross(
                row,
                evidence,
                target_self,
                target_self_category,
            )
        report_rows.append(
            {
                "model_run_id": row.get("model_run_id"),
                "label": row.get("label"),
                "input_id": row.get("input_id"),
                "source_config_id": row.get("source_config_id"),
                "target_config_id": row.get("target_config_id"),
                "is_self": row.get("is_self"),
                "validation_ready": row.get("validation_ready"),
                "state_model_fact_ready": row.get("state_model_fact_ready"),
                "final_state_match": row.get("final_state_match"),
                "closure_classification": category,
                "closure_classification_reason": reason,
                "closure_evidence": {
                    "validation_errors": row.get("validation_errors", []),
                    "target_self_label": target_self.get("label") if target_self else None,
                    "target_self_classification": target_self_category,
                    **evidence,
                },
            }
        )

    counts = dict(sorted(collections.Counter(str(row["closure_classification"]) for row in report_rows).items()))
    unrelated_count = int(counts.get(UNRELATED_SEMANTIC_MISMATCH, 0))
    not_ready_count = int(counts.get(NOT_READY, 0))
    return {
        "schema": "trace_sim.hicache.final_state_closure.v1",
        "prediction_count": len(report_rows),
        "raw_exact_count": sum(1 for row in report_rows if row.get("final_state_match") is True),
        "classification_counts": counts,
        "unrelated_semantic_mismatch_count": unrelated_count,
        "not_ready_count": not_ready_count,
        "review_ready": bool(report_rows) and unrelated_count == 0 and not_ready_count == 0,
        "rows": report_rows,
        "notes": [
            "Closure classification is diagnostic and never changes raw final-state exactness or readiness.",
            (
                "A readiness limitation requires complete state-model inputs, only the raw final-state "
                "mismatch blocker, and a final-DAG comparison with readiness-only shape differences."
            ),
            (
                "Cross schedule-sensitive classification requires an exact target-self result and explicit "
                "final-DAG alternate evidence."
            ),
            "Insufficient evidence remains unrelated_semantic_mismatch or not_ready.",
        ],
    }


def _classify_self(row: dict[str, Any], evidence: dict[str, Any]) -> tuple[str, str]:
    if _raw_exact_and_ready(row):
        return FINAL_STATE_EXACT, "raw final state is exact"
    if not _comparison_ready_except_final_state(row):
        return NOT_READY, "final-state comparison has blockers beyond the final-state mismatch"
    if readiness_only_shape(evidence):
        return (
            PREFETCH_READINESS_LIMITATION,
            "raw mismatch coincides with readiness-only final-DAG shape differences",
        )
    return UNRELATED_SEMANTIC_MISMATCH, "available evidence does not justify a known limitation category"


def _classify_cross(
    row: dict[str, Any],
    evidence: dict[str, Any],
    target_self: dict[str, Any] | None,
    target_self_category: str | None,
) -> tuple[str, str]:
    if _raw_exact_and_ready(row):
        return FINAL_STATE_EXACT, "raw final state is exact"
    if not _comparison_ready_except_final_state(row):
        return NOT_READY, "final-state comparison has blockers beyond the final-state mismatch"
    if readiness_only_shape(evidence):
        return (
            PREFETCH_READINESS_LIMITATION,
            "raw mismatch coincides with readiness-only final-DAG shape differences",
        )
    if target_self_category == PREFETCH_READINESS_LIMITATION and accepted_shape(evidence):
        return (
            PREFETCH_READINESS_LIMITATION,
            "cross mismatch inherits the target-self readiness limitation on an accepted modeled shape",
        )
    if (
        target_self is not None
        and target_self.get("final_state_match") is True
        and target_self_category == FINAL_STATE_EXACT
        and evidence.get("alternate_evidence_ready") is True
        and int(evidence.get("schedule_sensitive_mismatch_count") or 0) > 0
        and int(evidence.get("readiness_limitation_mismatch_count") or 0) == 0
        and accepted_shape(evidence)
    ):
        return (
            CROSS_SCHEDULE_SENSITIVE,
            "target-self is exact and alternate evidence covers cross arrival-sensitive shape differences",
        )
    return UNRELATED_SEMANTIC_MISMATCH, "available evidence does not justify a known limitation category"


def _raw_exact_and_ready(row: dict[str, Any]) -> bool:
    return bool(
        row.get("return_code") == 0
        and not row.get("skipped")
        and row.get("validation_ready") is True
        and row.get("state_model_fact_ready") is True
        and row.get("final_state_match") is True
    )


def _comparison_ready_except_final_state(row: dict[str, Any]) -> bool:
    errors = row.get("validation_errors")
    normalized_errors = {str(error) for error in errors} if isinstance(errors, list) else set()
    return bool(
        row.get("return_code") == 0
        and not row.get("skipped")
        and not row.get("dry_run")
        and row.get("state_model_fact_ready") is True
        and row.get("final_state_match") is False
        and normalized_errors == {_FINAL_STATE_MISMATCH}
    )
