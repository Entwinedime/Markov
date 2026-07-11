"""Timeline delta oracle derived from HiCache state snapshots."""

from __future__ import annotations

from typing import Any

from .event_delta import active_delta_state_keys, build_predicted_event_deltas, delta_rows_for_event_key
from .multiset import compare_delta_multisets, count_rows_by_transition_kind, summarize_delta_mismatches_by_kind
from ..snapshot.state import (
    derived_hicache_state_from_snapshot,
    event_base_name,
    snapshot_is_completed_state,
    snapshot_is_hiradix_cache_state,
    snapshot_logical_time_us,
    snapshot_timeline_sort_key,
    union_hicache_states,
)


def build_timeline_delta_validation(
    predicted_records: list[dict[str, Any]], snapshots: list[dict[str, Any]]
) -> dict[str, Any]:
    """Compare predictions with global state changes along object timelines."""

    active_state_keys = active_delta_state_keys(predicted_records)
    visible_state_keys = timeline_visible_state_keys(snapshots)
    comparison_state_keys = active_state_keys & visible_state_keys
    oracle = build_oracle_timeline_deltas(snapshots, comparison_state_keys)
    oracle_rows = list(oracle["rows"])
    predicted = build_predicted_event_deltas(predicted_records, comparison_state_keys)
    comparable = bool(oracle["rows"])
    mismatches = compare_delta_multisets(predicted["rows"], oracle_rows) if comparable else []
    model_extra_transition_count = sum(int(row.get("extra_in_predicted", 0) or 0) for row in mismatches)
    oracle_extra_transition_count = sum(int(row.get("missing_in_predicted", 0) or 0) for row in mismatches)
    model_transition_covered = comparable and model_extra_transition_count == 0
    predicted_counts = count_rows_by_transition_kind(predicted["rows"])
    oracle_counts = count_rows_by_transition_kind(oracle_rows)
    return {
        "ready": comparable,
        "match": model_transition_covered,
        "exact_match": comparable and not mismatches,
        "model_transition_covered": model_transition_covered,
        "model_extra_transition_count": model_extra_transition_count,
        "oracle_extra_transition_count": oracle_extra_transition_count,
        "oracle_transition_count": len(oracle_rows),
        "predicted_transition_count": len(predicted["rows"]),
        "compared_state_keys": sorted(comparison_state_keys),
        "ignored_unobservable_state_keys": sorted(active_state_keys - comparison_state_keys),
        "oracle_transition_count_by_kind": oracle_counts,
        "predicted_transition_count_by_kind": predicted_counts,
        "object_group_count": oracle["object_group_count"],
        "snapshot_count_with_object_id": oracle["snapshot_count_with_object_id"],
        "snapshot_count_without_object_id": oracle["snapshot_count_without_object_id"],
        "ignored_snapshot_count": oracle["ignored_snapshot_count"],
        "ignored_state_keys_without_predicted_transition": oracle["ignored_state_keys"],
        "mismatch_count": len(mismatches),
        "mismatch_totals_by_kind": summarize_delta_mismatches_by_kind(mismatches),
        "top_mismatches": mismatches[:20],
        "note": "Timeline delta comparison requires state_snapshot.object_id and compares transition kind/page multisets. match=true means every predicted transition is covered by the raw snapshot timeline; exact_match=false can still occur when sparse multi-process snapshots expose oracle-only transient state oscillations.",
    }


def timeline_visible_state_keys(snapshots: list[dict[str, Any]]) -> set[str]:
    """Return state keys made observable by completed raw snapshots."""

    visible: set[str] = set()
    for row in snapshots:
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
            continue
        if not snapshot_is_hiradix_cache_state(row, snapshot) or not snapshot_is_completed_state(row):
            continue
        state = derived_hicache_state_from_snapshot(snapshot)
        for key, value in state.items():
            if isinstance(value, list) and value:
                visible.add(str(key))
    return visible


def build_oracle_timeline_deltas(snapshots: list[dict[str, Any]], active_state_keys: set[str]) -> dict[str, Any]:
    """Build an oracle transition multiset along raw snapshot timelines."""

    timeline: list[tuple[tuple[int, int, int], tuple[str, str, str], dict[str, Any]]] = []
    snapshot_count_with_object_id = 0
    snapshot_count_without_object_id = 0
    ignored_snapshot_count = 0
    for row in snapshots:
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
            ignored_snapshot_count += 1
            continue
        if not snapshot_is_hiradix_cache_state(row, snapshot):
            ignored_snapshot_count += 1
            continue
        object_id = str(row.get("object_id") or snapshot.get("object_id") or "")
        if not object_id:
            snapshot_count_without_object_id += 1
            continue
        if not snapshot_is_completed_state(row):
            ignored_snapshot_count += 1
            continue
        snapshot_count_with_object_id += 1
        key = (str(row.get("trace_path") or ""), str(row.get("pid") or ""), object_id)
        timeline.append((snapshot_timeline_sort_key(row), key, row))

    rows: list[dict[str, Any]] = []
    ignored_state_keys: set[str] = set()
    object_states: dict[tuple[str, str, str], dict[str, Any]] = {}
    previous_union: dict[str, Any] = {}
    for _sort_key, key, row in sorted(timeline, key=lambda item: item[0]):
        object_states[key] = derived_hicache_state_from_snapshot(row.get("state_snapshot", {}))
        current_union = union_hicache_states(object_states.values())
        append_timeline_delta_rows(rows, ignored_state_keys, key, row, previous_union, current_union, active_state_keys)
        previous_union = current_union

    return {
        "rows": rows,
        "final_state": previous_union,
        "object_group_count": len(object_states),
        "snapshot_count_with_object_id": snapshot_count_with_object_id,
        "snapshot_count_without_object_id": snapshot_count_without_object_id,
        "ignored_snapshot_count": ignored_snapshot_count,
        "ignored_state_keys": sorted(ignored_state_keys),
    }


def append_timeline_delta_rows(
    rows: list[dict[str, Any]],
    ignored_state_keys: set[str],
    key: tuple[str, str, str],
    row: dict[str, Any],
    previous_union: dict[str, Any],
    current_union: dict[str, Any],
    active_state_keys: set[str],
) -> None:
    """Append one global union-state difference to timeline rows."""

    trace_path, pid, object_id = key
    delta_key = (
        trace_path,
        pid,
        str(row.get("tid") or ""),
        str(row.get("target_id") or ""),
        str(row.get("request_id") or ""),
        str(row.get("operation_id") or ""),
        snapshot_logical_time_us(row),
        event_base_name(str(row.get("source_event_name") or row.get("event_name") or "")),
    )
    delta_result = delta_rows_for_event_key(delta_key, previous_union, current_union, active_state_keys)
    for item in delta_result["rows"]:
        item["object_id"] = object_id
        item["source_event_name"] = str(row.get("source_event_name") or row.get("event_name") or "")
        rows.append(item)
    ignored_state_keys.update(delta_result["ignored_state_keys"])
