"""HiCache transition delta multiset 比较工具。"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class DeltaMultisetComparator:
    """按 `(transition_kind, page)` multiset 比较两组 delta rows。"""

    predicted_rows: list[dict[str, Any]]
    oracle_rows: list[dict[str, Any]]

    def mismatches(self) -> list[dict[str, Any]]:
        """返回 predicted/oracle multiset 差异行。"""

        predicted_counts = delta_multiset_counts(self.predicted_rows)
        oracle_counts = delta_multiset_counts(self.oracle_rows)
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


def compare_delta_multisets(
    predicted_rows: list[dict[str, Any]], oracle_rows: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    """按 `(transition_kind, page)` multiset 比较 predicted/oracle delta。"""

    return DeltaMultisetComparator(predicted_rows, oracle_rows).mismatches()


def delta_multiset_counts(rows: list[dict[str, Any]]) -> dict[tuple[str, str], int]:
    """统计 delta rows 中每个 transition/page 出现次数。"""

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
    """按 transition kind 汇总触达页数。"""

    counts: dict[str, int] = {}
    for row in rows:
        kind = str(row.get("transition_kind") or "")
        if not kind:
            continue
        counts[kind] = counts.get(kind, 0) + len([page for page in row.get("pages", []) if page is not None])
    return dict(sorted(counts.items()))


def mismatch_value_count(value: Any) -> int:
    """把 mismatch 字段里的 list/count 统一折算成数量。"""

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
    """按 transition kind 汇总 delta mismatch。"""

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
