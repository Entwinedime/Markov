"""transition exactness 的 delta row 归一化与比较。"""

from __future__ import annotations

import collections
from typing import Any

from ...oracle.diff.multiset import (
    compare_delta_multisets,
    count_rows_by_transition_kind,
    summarize_delta_mismatches_by_kind,
)
from ...oracle.snapshot.state import normalize_hicache_page_key, normalize_hicache_state_for_oracle_compare
from ..replay.record_schema import STATE_DELTA_KINDS, state_counts
from .oracle import SNAPSHOT_VISIBLE_STATE_KEYS, TRANSITION_COMPARABLE_STATE_KEYS


def comparable_model_delta_rows(rows: list[dict[str, Any]], page_key_mode: str) -> list[dict[str, Any]]:
    """筛出 snapshot 可见状态的模型 replay delta。"""

    visible_delta_kinds = {
        kind for state_key in TRANSITION_COMPARABLE_STATE_KEYS for kind in STATE_DELTA_KINDS[state_key]
    }
    return normalize_delta_rows_to_union_timeline(
        [row for row in rows if str(row.get("transition_kind") or "") in visible_delta_kinds],
        page_key_mode,
    )


def comparable_observed_delta_rows(rows: Any, page_key_mode: str) -> list[dict[str, Any]]:
    """规整 observed snapshot delta rows。"""

    if not isinstance(rows, list):
        return []
    visible_delta_kinds = {
        kind for state_key in TRANSITION_COMPARABLE_STATE_KEYS for kind in STATE_DELTA_KINDS[state_key]
    }
    return normalize_delta_rows_to_union_timeline(
        [row for row in rows if isinstance(row, dict) and str(row.get("transition_kind") or "") in visible_delta_kinds],
        page_key_mode,
    )


def normalize_delta_rows(rows: list[dict[str, Any]], page_key_mode: str) -> list[dict[str, Any]]:
    """归一化 delta rows 的 page key。"""

    result: list[dict[str, Any]] = []
    for row in rows:
        pages = row.get("pages")
        if not isinstance(pages, list):
            continue
        result.append(
            {
                **row,
                "pages": sorted(
                    {normalize_hicache_page_key(page, page_key_mode) for page in pages if page is not None}
                ),
            }
        )
    return result


def normalize_delta_rows_to_union_timeline(rows: list[dict[str, Any]], page_key_mode: str) -> list[dict[str, Any]]:
    """按归一化 page key 重建全局 union 口径的 delta rows。"""

    counts: collections.Counter[tuple[str, str]] = collections.Counter()
    result: list[dict[str, Any]] = []
    for row in normalize_delta_rows(rows, page_key_mode):
        kind = str(row.get("transition_kind") or "")
        state_key, direction = state_effect_from_delta_kind(kind)
        if not state_key or direction == 0:
            result.append(row)
            continue
        changed: list[str] = []
        for page in row.get("pages", []):
            key = (state_key, str(page))
            before = counts[key]
            after = max(0, before + direction)
            if after == 0:
                counts.pop(key, None)
            else:
                counts[key] = after
            if direction > 0 and before == 0 and after > 0:
                changed.append(str(page))
            elif direction < 0 and before > 0 and after == 0:
                changed.append(str(page))
        if changed:
            result.append({**row, "state_key": state_key, "pages": sorted(set(changed))})
    return result


def state_effect_from_delta_kind(kind: str) -> tuple[str, int]:
    """从 delta kind 反推 state key 和方向。"""

    for state_key, (add_kind, remove_kind) in STATE_DELTA_KINDS.items():
        if kind == add_kind:
            return state_key, 1
        if kind == remove_kind:
            return state_key, -1
    return "", 0


def final_state_comparison(
    model_final: dict[str, Any],
    observed_final: dict[str, Any],
    *,
    page_key_mode: str,
    sample_limit: int,
) -> dict[str, Any]:
    """比较 model/oracle final state 的 snapshot 可见字段。"""

    normalized_model = normalize_hicache_state_for_oracle_compare(model_final, page_key_mode)
    normalized_observed = normalize_hicache_state_for_oracle_compare(observed_final, page_key_mode)
    diffs: dict[str, Any] = {}
    match = True
    for key in SNAPSHOT_VISIBLE_STATE_KEYS:
        model_pages = page_set(normalized_model.get(key))
        observed_pages = page_set(normalized_observed.get(key))
        missing = sorted(observed_pages - model_pages)
        extra = sorted(model_pages - observed_pages)
        if missing or extra:
            match = False
        diffs[key] = {
            "match": not missing and not extra,
            "model_count": len(model_pages),
            "observed_count": len(observed_pages),
            "missing_in_model_count": len(missing),
            "extra_in_model_count": len(extra),
            "missing_in_model": missing[:sample_limit],
            "extra_in_model": extra[:sample_limit],
        }
    return {
        "match": match,
        "model_final_state_counts": state_counts(normalized_model),
        "observed_final_state_counts": state_counts(normalized_observed),
        "sets_diff_by_tier": diffs,
    }


def page_set(value: Any) -> set[str]:
    """把 page list 字段转成字符串集合。"""

    return {str(page) for page in value if page is not None} if isinstance(value, list) else set()


def compare_transition_kind_counts(
    model_rows: list[dict[str, Any]], observed_rows: list[dict[str, Any]]
) -> dict[str, Any]:
    """比较 transition kind 触达页数。"""

    model_counts = count_rows_by_transition_kind(model_rows)
    observed_counts = count_rows_by_transition_kind(observed_rows)
    by_kind: dict[str, Any] = {}
    match = True
    for kind in sorted(set(model_counts) | set(observed_counts)):
        model_count = int(model_counts.get(kind, 0))
        observed_count = int(observed_counts.get(kind, 0))
        if model_count != observed_count:
            match = False
        by_kind[kind] = {
            "match": model_count == observed_count,
            "model_count": model_count,
            "observed_count": observed_count,
            "missing_in_model": max(observed_count - model_count, 0),
            "extra_in_model": max(model_count - observed_count, 0),
        }
    return {"match": match, "by_kind": by_kind}


def compare_page_lifecycle_multiset(
    model_rows: list[dict[str, Any]],
    observed_rows: list[dict[str, Any]],
    *,
    sample_limit: int,
) -> dict[str, Any]:
    """比较 `(transition_kind, page)` multiset。"""

    mismatches = [
        {
            "transition_kind": row["transition_kind"],
            "page": row["page"],
            "model_count": row["predicted_count"],
            "observed_count": row["oracle_count"],
            "missing_in_model": row["missing_in_predicted"],
            "extra_in_model": row["extra_in_predicted"],
        }
        for row in compare_delta_multisets(model_rows, observed_rows)
    ]
    return {
        "match": not mismatches,
        "mismatch_count": len(mismatches),
        "mismatch_totals_by_kind": summarize_delta_mismatches_by_kind(
            mismatches,
            missing_key="missing_in_model",
            extra_key="extra_in_model",
            predicted_key="model_count",
            oracle_key="observed_count",
        ),
        "top_mismatches": mismatches[:sample_limit],
    }
