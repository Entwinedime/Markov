"""Closure classification for HiCache transition validation results."""

from __future__ import annotations

import collections
from typing import Any

from ..closure_evidence import (
    accepted_shape,
    final_dag_evidence,
    prediction_target_key,
    readiness_only_shape,
)
from ..final_state_closure import (
    CROSS_SCHEDULE_SENSITIVE as FINAL_STATE_CROSS_SCHEDULE_SENSITIVE,
    FINAL_STATE_EXACT,
    PREFETCH_READINESS_LIMITATION as FINAL_STATE_PREFETCH_READINESS_LIMITATION,
)


TRANSITION_EXACT = "transition_exact"
PREFETCH_READINESS_LIMITATION = "payload_only_prefetch_readiness_limitation"
SNAPSHOT_OBSERVABILITY = "snapshot_grouping_or_observability"
CROSS_SCHEDULE_SENSITIVE = "cross_arrival_schedule_sensitive"
UNRELATED_SEMANTIC_MISMATCH = "unrelated_semantic_mismatch"
NOT_READY = "not_ready"

_SNAPSHOT_CLASSIFICATIONS = {"state_marker_only", "transition_grouping"}


def build_transition_closure_report(
    rows: list[dict[str, Any]],
    final_dag_rows: dict[str, dict[str, Any]],
    final_state_closure_rows: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    """Classify raw transition outcomes without changing exactness semantics."""

    self_rows = {prediction_target_key(row): row for row in rows if row.get("is_self") is True}
    final_state_self_classifications = {
        prediction_target_key(row): str(row.get("closure_classification") or "")
        for row in final_state_closure_rows.values()
        if row.get("is_self") is True
    }
    self_classifications: dict[tuple[str, str], str] = {}
    for key, row in self_rows.items():
        category, _ = _classify_self(
            row,
            final_dag_evidence(row, final_dag_rows),
            _final_state_category(row, final_state_closure_rows),
        )
        self_classifications[key] = category

    report_rows: list[dict[str, Any]] = []
    for row in rows:
        evidence = final_dag_evidence(row, final_dag_rows)
        final_state_category = _final_state_category(row, final_state_closure_rows)
        target_key = prediction_target_key(row)
        target_self_final_state_category = final_state_self_classifications.get(target_key)
        if row.get("is_self") is True:
            category, reason = _classify_self(row, evidence, final_state_category)
        else:
            target_self = self_rows.get(target_key)
            target_self_category = self_classifications.get(target_key)
            category, reason = _classify_cross(
                row,
                evidence,
                target_self,
                target_self_category,
                final_state_category,
                target_self_final_state_category,
            )
        closure_evidence = _closure_evidence(
            row,
            evidence,
            self_rows.get(target_key),
            self_classifications.get(target_key),
            final_state_category,
            target_self_final_state_category,
        )
        row["closure_classification"] = category
        row["closure_classification_reason"] = reason
        row["closure_evidence"] = closure_evidence
        report_rows.append(
            {
                "model_run_id": row.get("model_run_id"),
                "label": row.get("label"),
                "input_id": row.get("input_id"),
                "source_config_id": row.get("source_config_id"),
                "target_config_id": row.get("target_config_id"),
                "is_self": row.get("is_self"),
                "ready": row.get("ready"),
                "exact": row.get("exact"),
                "closure_classification": category,
                "closure_classification_reason": reason,
                "closure_evidence": closure_evidence,
            }
        )

    counts = dict(sorted(collections.Counter(str(row["closure_classification"]) for row in report_rows).items()))
    unrelated_count = int(counts.get(UNRELATED_SEMANTIC_MISMATCH, 0))
    not_ready_count = int(counts.get(NOT_READY, 0))
    return {
        "schema": "trace_sim.hicache.transition_closure.v1",
        "prediction_count": len(report_rows),
        "raw_exact_count": sum(1 for row in report_rows if row.get("exact") is True),
        "classification_counts": counts,
        "unrelated_semantic_mismatch_count": unrelated_count,
        "not_ready_count": not_ready_count,
        "review_ready": bool(report_rows) and unrelated_count == 0 and not_ready_count == 0,
        "rows": report_rows,
        "notes": [
            "Closure classification is diagnostic and never changes raw transition exactness.",
            (
                "Readiness limitation requires complete transition inputs, an accepted final-state closure "
                "classification, and direct or target-self final-DAG readiness evidence."
            ),
            (
                "Cross schedule-sensitive classification requires target-self transition evidence and "
                "final-DAG alternate evidence."
            ),
            "Insufficient evidence is classified as unrelated_semantic_mismatch rather than accepted.",
        ],
    }


def _classify_self(
    row: dict[str, Any],
    evidence: dict[str, Any],
    final_state_category: str,
) -> tuple[str, str]:
    if row.get("exact") is True:
        return TRANSITION_EXACT, "raw transition multiset and counts are exact"
    if _readiness_limitation(row, evidence, final_state_category, None, None):
        return (
            PREFETCH_READINESS_LIMITATION,
            "transition inputs are ready and the only comparison gate is the final-state readiness limitation",
        )
    if row.get("ready") is not True:
        return NOT_READY, "transition comparison has blockers beyond an accepted final-state limitation"
    if _snapshot_observability(row):
        return SNAPSHOT_OBSERVABILITY, "mismatch is limited to snapshot marker grouping or observability"
    return UNRELATED_SEMANTIC_MISMATCH, "available evidence does not justify a known limitation category"


def _classify_cross(
    row: dict[str, Any],
    evidence: dict[str, Any],
    target_self: dict[str, Any] | None,
    target_self_category: str | None,
    final_state_category: str,
    target_self_final_state_category: str | None,
) -> tuple[str, str]:
    if row.get("exact") is True:
        return TRANSITION_EXACT, "raw transition multiset and counts are exact"
    if _readiness_limitation(
        row,
        evidence,
        final_state_category,
        target_self_category,
        target_self_final_state_category,
    ):
        return (
            PREFETCH_READINESS_LIMITATION,
            "transition inputs are ready and direct or target-self evidence identifies a readiness limitation",
        )
    if _cross_schedule_limitation(row, evidence, target_self, final_state_category):
        return (
            CROSS_SCHEDULE_SENSITIVE,
            "target-self transition is exact and final-state closure identifies cross arrival sensitivity",
        )
    if row.get("ready") is not True:
        return NOT_READY, "transition comparison has blockers beyond an accepted final-state limitation"
    if _snapshot_observability(row):
        return SNAPSHOT_OBSERVABILITY, "mismatch is limited to snapshot marker grouping or observability"
    if (
        target_self is not None
        and target_self.get("exact") is True
        and evidence.get("alternate_evidence_ready") is True
        and int(evidence.get("schedule_sensitive_mismatch_count") or 0) > 0
        and accepted_shape(evidence)
    ):
        return (
            CROSS_SCHEDULE_SENSITIVE,
            "target-self transition is exact and final-DAG alternate evidence covers arrival-sensitive effects",
        )
    return UNRELATED_SEMANTIC_MISMATCH, "available evidence does not justify a known limitation category"


def _snapshot_observability(row: dict[str, Any]) -> bool:
    return str(row.get("classification") or "") in _SNAPSHOT_CLASSIFICATIONS


def _readiness_limitation(
    row: dict[str, Any],
    evidence: dict[str, Any],
    final_state_category: str,
    target_self_transition_category: str | None,
    target_self_final_state_category: str | None,
) -> bool:
    if (
        row.get("ready") is True
        and row.get("final_state_exact") is True
        and final_state_category == FINAL_STATE_EXACT
        and str(row.get("classification") or "") == "physical_candidate"
        and readiness_only_shape(evidence)
    ):
        return True
    if not _transition_ready_except_final_state(row):
        return False
    if final_state_category == FINAL_STATE_PREFETCH_READINESS_LIMITATION and readiness_only_shape(evidence):
        return True
    return bool(
        target_self_transition_category == PREFETCH_READINESS_LIMITATION
        and target_self_final_state_category == FINAL_STATE_PREFETCH_READINESS_LIMITATION
        and accepted_shape(evidence)
    )


def _cross_schedule_limitation(
    row: dict[str, Any],
    evidence: dict[str, Any],
    target_self: dict[str, Any] | None,
    final_state_category: str,
) -> bool:
    return bool(
        _transition_ready_except_final_state(row)
        and final_state_category == FINAL_STATE_CROSS_SCHEDULE_SENSITIVE
        and target_self is not None
        and target_self.get("exact") is True
        and evidence.get("alternate_evidence_ready") is True
        and int(evidence.get("schedule_sensitive_mismatch_count") or 0) > 0
        and accepted_shape(evidence)
    )


def _transition_ready_except_final_state(row: dict[str, Any]) -> bool:
    return bool(
        row.get("return_code") == 0
        and not row.get("skipped")
        and row.get("model_transition_self_check_ready") is True
        and row.get("oracle_ready") is True
        and row.get("final_state_exact") is False
        and row.get("failure_classification") == "real_semantic_mismatch_or_final_state_regression"
        and isinstance(row.get("transition_count_exact"), bool)
        and isinstance(row.get("page_lifecycle_multiset_exact"), bool)
    )


def _final_state_category(
    row: dict[str, Any],
    final_state_closure_rows: dict[str, dict[str, Any]],
) -> str:
    model_run_id = str(row.get("model_run_id") or "")
    closure_row = final_state_closure_rows.get(model_run_id, {})
    return str(closure_row.get("closure_classification") or "")


def _closure_evidence(
    row: dict[str, Any],
    final_dag_evidence: dict[str, Any],
    target_self: dict[str, Any] | None,
    target_self_transition_category: str | None,
    final_state_category: str,
    target_self_final_state_category: str | None,
) -> dict[str, Any]:
    return {
        "transition_family": row.get("transition_family"),
        "transition_taxonomy_classification": row.get("classification"),
        "target_self_label": target_self.get("label") if target_self else None,
        "target_self_exact": target_self.get("exact") if target_self else None,
        "target_self_transition_closure_classification": target_self_transition_category,
        "model_transition_self_check_ready": row.get("model_transition_self_check_ready"),
        "oracle_ready": row.get("oracle_ready"),
        "final_state_exact": row.get("final_state_exact"),
        "failure_classification": row.get("failure_classification"),
        "final_state_closure_classification": final_state_category,
        "target_self_final_state_closure_classification": target_self_final_state_category,
        **final_dag_evidence,
    }
