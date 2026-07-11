"""Multiset comparison helpers for HiCache transition deltas."""

from __future__ import annotations

from typing import Any


def compare_delta_multisets(
    predicted_rows: list[dict[str, Any]], oracle_rows: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    """Compare predicted and oracle ``(transition_kind, page)`` multisets."""

    predicted_counts = delta_multiset_counts(predicted_rows)
    oracle_counts = delta_multiset_counts(oracle_rows)
    mismatches: list[dict[str, Any]] = []
    for key in sorted(set(predicted_counts) | set(oracle_counts)):
        predicted_count = predicted_counts.get(key, 0)
        oracle_count = oracle_counts.get(key, 0)
        if predicted_count == oracle_count:
            continue
        transition_kind, page = key
        mismatches.append(
            {
                "transition_kind": transition_kind,
                "page": page,
                "predicted_count": predicted_count,
                "oracle_count": oracle_count,
                "missing_in_predicted": max(oracle_count - predicted_count, 0),
                "extra_in_predicted": max(predicted_count - oracle_count, 0),
            }
        )
    return mismatches


def delta_multiset_counts(rows: list[dict[str, Any]]) -> dict[tuple[str, str], int]:
    """Count each ``(transition_kind, page)`` occurrence in delta rows."""

    counts: dict[tuple[str, str], int] = {}
    for row in rows:
        kind = str(row.get("transition_kind") or "")
        if not kind:
            continue
        for page in row.get("pages", []):
            if page is None:
                continue
            key = (kind, str(page))
            counts[key] = counts.get(key, 0) + 1
    return counts


def count_rows_by_transition_kind(rows: list[dict[str, Any]]) -> dict[str, int]:
    """Count touched pages by transition kind."""

    counts: dict[str, int] = {}
    for row in rows:
        kind = str(row.get("transition_kind") or "")
        if not kind:
            continue
        counts[kind] = counts.get(kind, 0) + len([page for page in row.get("pages", []) if page is not None])
    return dict(sorted(counts.items()))


def mismatch_value_count(value: Any) -> int:
    """Normalize a list-valued or scalar mismatch field to a count."""

    if isinstance(value, list):
        return len([item for item in value if item is not None])
    try:
        return int(value or 0)
    except (TypeError, ValueError):
        return 0


def summarize_delta_mismatches_by_kind(
    mismatches: list[dict[str, Any]],
    *,
    missing_key: str = "missing_in_predicted",
    extra_key: str = "extra_in_predicted",
    predicted_key: str = "predicted_count",
    oracle_key: str = "oracle_count",
) -> dict[str, dict[str, int]]:
    """Aggregate delta mismatches by transition kind."""

    summary: dict[str, dict[str, int]] = {}
    for row in mismatches:
        kind = str(row.get("transition_kind") or "")
        if not kind:
            continue
        item = summary.setdefault(
            kind,
            {
                "mismatch_rows": 0,
                missing_key: 0,
                extra_key: 0,
                predicted_key: 0,
                oracle_key: 0,
            },
        )
        item["mismatch_rows"] += 1
        item[missing_key] += mismatch_value_count(row.get(missing_key))
        item[extra_key] += mismatch_value_count(row.get(extra_key))
        item[predicted_key] += mismatch_value_count(row.get(predicted_key))
        item[oracle_key] += mismatch_value_count(row.get(oracle_key))
    return {kind: summary[kind] for kind in sorted(summary)}
