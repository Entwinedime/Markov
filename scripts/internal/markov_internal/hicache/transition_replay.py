"""Replay predicted HiCache transition records into comparable state deltas."""

from __future__ import annotations

import collections
from typing import Any

from .oracle_state import normalize_hicache_page_key
from .transition_record_schema import (
    ACTIVE_STATE_KEYS,
    SELF_CHECK_HARD_STATE_KEYS,
    STATE_DELTA_KINDS,
    record_pages,
    state_counts,
)


LOCK_ACQUIRE_KINDS = {
    "acquire_request_ref",
    "enqueue_loadback",
    "enqueue_storage_backup",
    "enqueue_write_through_backup",
    "enqueue_writeback",
}

LOCK_RELEASE_BY_KIND = {
    "release_request_ref": "request_ref",
    "complete_loadback": "loadback",
    "complete_storage_backup": "storage",
    "complete_write_through_backup": "write_through_backup",
    "complete_writeback": "writeback",
    "cancel_writeback": "writeback",
}


def replay_predicted_records(records: list[dict[str, Any]], *, sample_limit: int) -> dict[str, Any]:
    """按 transition 顺序回放模型侧 active state。"""

    state: dict[str, set[str]] = {key: set() for key in ACTIVE_STATE_KEYS}
    page_hit_counts: collections.Counter[str] = collections.Counter()
    ref_counts: collections.Counter[str] = collections.Counter()
    delta_rows: list[dict[str, Any]] = []
    violations: list[dict[str, Any]] = []
    ref_issues: list[dict[str, Any]] = []
    noop_rows: list[dict[str, Any]] = []

    for ordinal, record in enumerate(records):
        kind = str(record.get("transition_kind") or "")
        pages = record_pages(record)
        before_delta_count = len(delta_rows)
        if kind in {"add_l1_residency", "restore_l1_residency", "promote_visible_prefix_to_l1"}:
            add_pages(state, delta_rows, "l1_resident_pages", pages, record, ordinal)
            remove_pages(state, delta_rows, "evicted_pages", pages, record, ordinal, strict=False)
        elif kind == "mark_dirty":
            add_pages(state, delta_rows, "dirty_pages", pages, record, ordinal)
        elif kind in {"commit_host_storage_backup", "commit_host_backup"}:
            add_pages(state, delta_rows, "l2_resident_pages", pages, record, ordinal)
            add_pages(state, delta_rows, "backuped_pages", pages, record, ordinal)
            if kind == "commit_host_storage_backup":
                add_pages(state, delta_rows, "l3_resident_pages", pages, record, ordinal)
            remove_pages(state, delta_rows, "dirty_pages", pages, record, ordinal, strict=False)
        elif kind == "apply_prefetch_host_visibility":
            add_pages(state, delta_rows, "l2_resident_pages", pages, record, ordinal)
            add_pages(state, delta_rows, "backuped_pages", pages, record, ordinal)
            add_pages(state, delta_rows, "l3_resident_pages", pages, record, ordinal)
            add_pages(
                state,
                delta_rows,
                "evicted_pages",
                [page for page in pages if page not in state["l1_resident_pages"]],
                record,
                ordinal,
            )
        elif kind == "evict_l1_node":
            remove_pages(
                state,
                delta_rows,
                "l1_resident_pages",
                pages,
                record,
                ordinal,
                strict=True,
                violations=violations,
            )
            remove_pages(state, delta_rows, "dirty_pages", pages, record, ordinal, strict=False)
            backed_pages = [page for page in pages if page in state["l2_resident_pages"] or page in state["backuped_pages"]]
            add_pages(state, delta_rows, "evicted_pages", backed_pages, record, ordinal)
        elif kind == "evict_host_node":
            for key in ("l2_resident_pages", "l3_resident_pages", "backuped_pages", "evicted_pages", "locked_pages"):
                remove_pages(state, delta_rows, key, pages, record, ordinal, strict=False)
            for page in pages:
                ref_counts.pop(page, None)
        elif kind == "prefetch_planned":
            add_pages(state, delta_rows, "prefetch_planned_pages", pages, record, ordinal)
        elif kind == "prefetch_ready":
            add_pages(state, delta_rows, "prefetch_ready_pages", pages, record, ordinal)
        elif kind in {"prefetch_revoked", "prefetch_suppressed"}:
            add_pages(state, delta_rows, "prefetch_suppressed_pages", pages, record, ordinal)
        elif kind == "prefetch_timeout_incomplete":
            add_pages(state, delta_rows, "prefetch_late_pages", pages, record, ordinal)
        elif kind == "prefetch_terminated":
            pass
        elif kind == "increment_hit_count":
            for page in pages:
                page_hit_counts[page] += 1
        elif kind == "enqueue_writeback":
            add_pages(state, delta_rows, "pending_writeback_pages", pages, record, ordinal)
            acquire_refs(state, delta_rows, ref_counts, pages, record, ordinal)
        elif kind in LOCK_ACQUIRE_KINDS:
            acquire_refs(state, delta_rows, ref_counts, pages, record, ordinal)
        elif kind in LOCK_RELEASE_BY_KIND:
            if kind in {"complete_writeback", "cancel_writeback"}:
                remove_pages(state, delta_rows, "pending_writeback_pages", pages, record, ordinal, strict=False)
            release_refs(state, delta_rows, ref_counts, pages, record, ordinal, ref_issues)
        elif kind in {"enqueue_storage_backup", "enqueue_write_through_backup", "enqueue_loadback", "complete_storage_backup", "complete_loadback"}:
            pass
        elif kind:
            violations.append(
                {
                    "kind": "unknown_transition_kind",
                    "ordinal": ordinal,
                    "transition_kind": kind,
                    "source_event_index": record.get("source_event_index"),
                }
            )
        if len(delta_rows) == before_delta_count and pages and kind not in {
            "increment_hit_count",
            "complete_storage_backup",
            "complete_loadback",
            "complete_writeback",
            "cancel_writeback",
            "prefetch_terminated",
        }:
            noop_rows.append(
                {
                    "ordinal": ordinal,
                    "transition_kind": kind,
                    "source_event_index": record.get("source_event_index"),
                    "page_count": len(pages),
                }
            )

    final_state = {key: sorted(values) for key, values in state.items()}
    final_state["page_hit_counts"] = dict(sorted(page_hit_counts.items()))
    return {
        "final_state": final_state,
        "delta_rows": delta_rows,
        "state_constraint_violations": violations[: max(sample_limit, len(violations))],
        "ref_balance_issues": ref_issues[: max(sample_limit, len(ref_issues))],
        "derived_noop_transitions": noop_rows[: max(sample_limit, len(noop_rows))],
    }


def add_pages(
    state: dict[str, set[str]],
    delta_rows: list[dict[str, Any]],
    key: str,
    pages: list[str],
    record: dict[str, Any],
    ordinal: int,
) -> None:
    """向某个状态集合添加页，并记录实际 delta。"""

    changed = [page for page in pages if page not in state[key]]
    if not changed:
        return
    state[key].update(changed)
    emit_delta(delta_rows, key, True, changed, record, ordinal)


def remove_pages(
    state: dict[str, set[str]],
    delta_rows: list[dict[str, Any]],
    key: str,
    pages: list[str],
    record: dict[str, Any],
    ordinal: int,
    *,
    strict: bool,
    violations: list[dict[str, Any]] | None = None,
) -> None:
    """从某个状态集合删除页，并记录实际 delta。"""

    missing = [page for page in pages if page not in state[key]]
    if strict and missing and violations is not None:
        violations.append(
            {
                "kind": "remove_missing_page",
                "state_key": key,
                "ordinal": ordinal,
                "transition_kind": record.get("transition_kind"),
                "source_event_index": record.get("source_event_index"),
                "missing_pages_sample": missing[:8],
                "missing_count": len(missing),
            }
        )
    changed = [page for page in pages if page in state[key]]
    if not changed:
        return
    state[key].difference_update(changed)
    emit_delta(delta_rows, key, False, changed, record, ordinal)


def acquire_refs(
    state: dict[str, set[str]],
    delta_rows: list[dict[str, Any]],
    ref_counts: collections.Counter[str],
    pages: list[str],
    record: dict[str, Any],
    ordinal: int,
) -> None:
    """按 page 维护简化 ref 计数。"""

    newly_locked: list[str] = []
    for page in pages:
        ref_counts[page] += 1
        if ref_counts[page] == 1 and page not in state["locked_pages"]:
            state["locked_pages"].add(page)
            newly_locked.append(page)
    if newly_locked:
        emit_delta(delta_rows, "locked_pages", True, newly_locked, record, ordinal)


def release_refs(
    state: dict[str, set[str]],
    delta_rows: list[dict[str, Any]],
    ref_counts: collections.Counter[str],
    pages: list[str],
    record: dict[str, Any],
    ordinal: int,
    ref_issues: list[dict[str, Any]],
) -> None:
    """释放简化 ref 计数，并报告负计数风险。"""

    cleared: list[str] = []
    missing: list[str] = []
    for page in pages:
        if ref_counts[page] <= 0:
            missing.append(page)
            ref_counts.pop(page, None)
            continue
        ref_counts[page] -= 1
        if ref_counts[page] <= 0:
            ref_counts.pop(page, None)
            if page in state["locked_pages"]:
                state["locked_pages"].remove(page)
                cleared.append(page)
    if missing:
        ref_issues.append(
            {
                "kind": "release_ref_without_replay_acquire",
                "ordinal": ordinal,
                "transition_kind": record.get("transition_kind"),
                "source_event_index": record.get("source_event_index"),
                "missing_pages_sample": missing[:8],
                "missing_count": len(missing),
            }
        )
    if cleared:
        emit_delta(delta_rows, "locked_pages", False, cleared, record, ordinal)


def emit_delta(
    delta_rows: list[dict[str, Any]],
    state_key: str,
    is_add: bool,
    pages: list[str],
    record: dict[str, Any],
    ordinal: int,
) -> None:
    """记录 replay 产生的可比较状态 delta。"""

    kinds = STATE_DELTA_KINDS.get(state_key)
    if kinds is None or not pages:
        return
    transition_kind = kinds[0] if is_add else kinds[1]
    delta_rows.append(
        {
            "transition_ordinal": ordinal,
            "source_transition_kind": record.get("transition_kind"),
            "transition_kind": transition_kind,
            "state_key": state_key,
            "pages": sorted(set(pages)),
            "cache_scope": record.get("cache_scope") or "",
            "request_id": record.get("request_id") or "",
            "operation_id": record.get("operation_id") or "",
            "source_event_index": record.get("source_event_index"),
            "source_event_name": record.get("source_event_name") or "",
            "event_base_name": record.get("event_base_name") or "",
            "ts": record.get("ts"),
        }
    )


def compare_replay_final_state(replay: dict[str, Any], final_state: dict[str, Any], *, sample_limit: int) -> dict[str, Any]:
    """比较 replay final state 和模型 summary final state。"""

    replay_final = replay["final_state"]
    diffs: dict[str, Any] = {}
    strict_active_sets_match = True
    hard_active_sets_match = True
    for key in ACTIVE_STATE_KEYS:
        model_pages = normalize_page_set(final_state.get(key, []), "raw")
        replay_pages = normalize_page_set(replay_final.get(key, []), "raw")
        missing = sorted(model_pages - replay_pages)
        extra = sorted(replay_pages - model_pages)
        if missing or extra:
            strict_active_sets_match = False
            if key in SELF_CHECK_HARD_STATE_KEYS:
                hard_active_sets_match = False
        diffs[key] = {
            "match": not missing and not extra,
            "model_count": len(model_pages),
            "replayed_count": len(replay_pages),
            "missing_in_replay": missing[:sample_limit],
            "extra_in_replay": extra[:sample_limit],
            "missing_count": len(missing),
            "extra_count": len(extra),
        }
    model_hits = (
        {str(key): int(value) for key, value in (final_state.get("page_hit_counts") or {}).items()}
        if isinstance(final_state.get("page_hit_counts"), dict)
        else {}
    )
    replay_hits = (
        {str(key): int(value) for key, value in (replay_final.get("page_hit_counts") or {}).items()}
        if isinstance(replay_final.get("page_hit_counts"), dict)
        else {}
    )
    hit_mismatch = compare_counter_dicts(model_hits, replay_hits, sample_limit=sample_limit)
    return {
        "replay_final_state_match": hard_active_sets_match,
        "active_set_replay_match": hard_active_sets_match,
        "strict_active_set_replay_match": strict_active_sets_match,
        "strict_replay_final_state_match": strict_active_sets_match and hit_mismatch["match"],
        "page_hit_counts_match": hit_mismatch["match"],
        "replayed_final_state_counts": state_counts(replay_final),
        "model_final_state_counts": state_counts(final_state),
        "sets_diff_by_tier": diffs,
        "page_hit_counts_diff": hit_mismatch,
        "unreplayed_state_transition_count": 0,
        "advisory_replay_state_keys": {
            "locked_pages": "predicted transition trace does not currently expose every source_actual lock/ref or prefetch anchor protection mutation; strict mismatch is reported but stable state exactness does not gate on it.",
            "page_hit_counts": "hit count is diagnostic metadata and is reported separately from active-state replay.",
        },
    }


def normalize_page_set(value: Any, page_key_mode: str) -> set[str]:
    """把页面列表归一化成集合。"""

    if not isinstance(value, list):
        return set()
    return {normalize_hicache_page_key(page, page_key_mode) for page in value if page is not None}


def compare_counter_dicts(expected: dict[str, int], actual: dict[str, int], *, sample_limit: int) -> dict[str, Any]:
    """比较两个 page counter 字典。"""

    mismatches: list[dict[str, Any]] = []
    for key in sorted(set(expected) | set(actual)):
        left = expected.get(key, 0)
        right = actual.get(key, 0)
        if left != right:
            mismatches.append({"page": key, "model_count": left, "replayed_count": right})
    return {
        "match": not mismatches,
        "mismatch_count": len(mismatches),
        "mismatches": mismatches[:sample_limit],
    }
