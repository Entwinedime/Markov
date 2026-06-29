"""final-state mismatch 解释辅助工具。"""

from __future__ import annotations

from typing import Any


def first_hicache_mismatch(sets_diff: dict[str, Any], predicted_records: list[dict[str, Any]] | None = None) -> dict[str, Any] | None:
    """返回第一个 final-state mismatch 及其候选 transition 证据。"""

    predicted_records = predicted_records or []
    for key, value in sets_diff.items():
        if isinstance(value, dict) and not value.get("match", False):
            missing = value.get("missing_in_model", [])
            extra = value.get("extra_in_model", [])
            page = str((missing or extra or [""])[0])
            return {
                "tier": key,
                "page": page,
                "missing_in_model": missing,
                "extra_in_model": extra,
                "candidate_transition": first_transition_touching_page(predicted_records, page),
            }
    return None


def first_transition_touching_page(records: list[dict[str, Any]], page: str) -> dict[str, Any] | None:
    """查找第一条触达指定 page 的 predicted transition。"""

    if not page:
        return None
    for record in records:
        pages = record.get("target_page_set")
        if not isinstance(pages, list) or page not in {str(item) for item in pages}:
            continue
        return {
            "source_fact_id": record.get("source_fact_id"),
            "source_event_index": record.get("source_event_index"),
            "source_event_name": record.get("source_event_name"),
            "request_id": record.get("request_id"),
            "operation_id": record.get("operation_id"),
            "transition_kind": record.get("transition_kind"),
            "predicted_operation_kind": record.get("predicted_operation_kind"),
            "decision_reason": record.get("decision_reason"),
        }
    return None
