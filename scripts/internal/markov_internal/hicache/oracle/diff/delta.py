"""HiCache state snapshot delta oracle 辅助工具。"""

from __future__ import annotations

from typing import Any

from ..snapshot.records import page_set_from_predicted_record
from ..snapshot.state import (
    derived_hicache_state_from_snapshot,
    event_base_name,
    event_phase,
    optional_float,
    snapshot_is_completed_state,
    snapshot_is_hiradix_cache_state,
    snapshot_logical_time_us,
    snapshot_sort_key,
    snapshot_timeline_sort_key,
    union_hicache_states,
)


DELTA_KIND_BY_STATE_KEY: dict[str, tuple[str, str]] = {
    "l1_resident_pages": ("add_l1_resident", "remove_l1_resident"),
    "l2_resident_pages": ("add_l2_resident", "remove_l2_resident"),
    "dirty_pages": ("mark_dirty", "clear_dirty"),
    "backuped_pages": ("mark_backuped", "clear_backuped"),
    "evicted_pages": ("mark_evicted", "clear_evicted"),
    "locked_pages": ("mark_locked", "clear_locked"),
}


def build_event_delta_validation(
    predicted_records: list[dict[str, Any]], snapshots: list[dict[str, Any]]
) -> dict[str, Any]:
    """把 state snapshot 的 start/end 差分变成事件级 oracle。"""

    active_state_keys = active_delta_state_keys(predicted_records)
    oracle = build_oracle_event_deltas(snapshots, active_state_keys)
    predicted = build_predicted_event_deltas(predicted_records)
    predicted_event_keys = {str(row.get("event_key") or "") for row in predicted["rows"]}
    oracle_event_keys = {str(row.get("event_key") or "") for row in oracle["exclusive_rows"]}
    shared_event_keys = sorted(predicted_event_keys & oracle_event_keys)
    comparable = bool(shared_event_keys)
    mismatches = (
        compare_event_delta_rows(predicted["rows"], oracle["exclusive_rows"], set(shared_event_keys))
        if comparable
        else []
    )
    return {
        "ready": bool(oracle["inclusive_rows"]),
        "comparable": comparable,
        "match": comparable and not mismatches,
        "oracle_paired_event_count": oracle["paired_event_count"],
        "oracle_transition_count": len(oracle["exclusive_rows"]),
        "inclusive_oracle_transition_count": len(oracle["inclusive_rows"]),
        "predicted_comparable_transition_count": len(predicted["rows"]),
        "shared_event_key_count": len(shared_event_keys),
        "exclusive_oracle_event_key_count": len(oracle_event_keys),
        "predicted_event_key_count": len(predicted_event_keys),
        "exclusive_oracle_event_keys_missing_predicted": sorted(oracle_event_keys - predicted_event_keys)[:50],
        "predicted_event_keys_without_exclusive_oracle": sorted(predicted_event_keys - oracle_event_keys)[:50],
        "oracle_transition_count_by_kind": count_rows_by_transition_kind(oracle["exclusive_rows"]),
        "inclusive_oracle_transition_count_by_kind": count_rows_by_transition_kind(oracle["inclusive_rows"]),
        "predicted_comparable_transition_count_by_kind": count_rows_by_transition_kind(predicted["rows"]),
        "unpaired_snapshot_count": oracle["unpaired_snapshot_count"],
        "nested_event_count": oracle["nested_event_count"],
        "nested_oracle_transition_count": oracle["nested_transition_count"],
        "ignored_state_keys_without_predicted_transition": oracle["ignored_state_keys"],
        "mismatch_count": len(mismatches),
        "mismatch_totals_by_kind": summarize_delta_mismatches_by_kind(mismatches),
        "top_mismatches": mismatches[:20],
        "note": "Exact event delta comparison is intended for self-config prediction; cross-config prediction should use final-state and policy oracle fields.",
    }


def build_timeline_delta_validation(
    predicted_records: list[dict[str, Any]], snapshots: list[dict[str, Any]]
) -> dict[str, Any]:
    """按 cache object 的 snapshot 时间线生成一次性状态变化 oracle。"""

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
        "raw_mismatch_count": len(mismatches),
        "raw_top_mismatches": mismatches[:20],
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
    """返回 raw snapshot timeline 实际暴露过的 state key。"""

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
    """沿 raw state snapshot 时间线构造 oracle transition multiset。"""

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
    previous_union: dict[str, Any] | None = {}
    object_groups_seen: set[tuple[str, str, str]] = set()
    for _sort_key, key, row in sorted(timeline, key=lambda item: item[0]):
        object_groups_seen.add(key)
        object_states[key] = derived_hicache_state_from_snapshot(row.get("state_snapshot", {}))
        current_union = union_hicache_states(object_states.values())
        if previous_union is not None:
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
        previous_union = current_union

    return {
        "rows": rows,
        "final_state": previous_union or {},
        "object_group_count": len(object_groups_seen),
        "snapshot_count_with_object_id": snapshot_count_with_object_id,
        "snapshot_count_without_object_id": snapshot_count_without_object_id,
        "ignored_snapshot_count": ignored_snapshot_count,
        "ignored_state_keys": sorted(ignored_state_keys),
    }


def active_delta_state_keys(predicted_records: list[dict[str, Any]]) -> set[str]:
    """根据模型实际输出决定事件级 oracle 需要比较哪些状态集合。"""

    active_kinds = {
        str(record.get("transition_kind") or "") for record in predicted_records if isinstance(record, dict)
    }
    active: set[str] = set()
    for state_key, kinds in DELTA_KIND_BY_STATE_KEY.items():
        if any(kind in active_kinds for kind in kinds):
            active.add(state_key)
    return active


def build_oracle_event_deltas(snapshots: list[dict[str, Any]], active_state_keys: set[str]) -> dict[str, Any]:
    """把 start/end 成对 snapshot 转成事件包围区间差分。"""

    groups: dict[tuple[str, str, str, str, str, str, int, str], dict[str, dict[str, Any]]] = {}
    for row in snapshots:
        source_name = str(row.get("source_event_name") or row.get("event_name") or "")
        phase = event_phase(source_name)
        if phase not in {"start", "end"}:
            continue
        key = snapshot_event_key(row, source_name)
        group = groups.setdefault(key, {})
        current = group.get(phase)
        if current is None or snapshot_sort_key(row) >= snapshot_sort_key(current):
            group[phase] = row

    inclusive_rows: list[dict[str, Any]] = []
    exclusive_rows: list[dict[str, Any]] = []
    paired = 0
    unpaired = 0
    nested_event_count = 0
    nested_transition_count = 0
    ignored_state_keys: set[str] = set()
    paired_groups: list[tuple[tuple[str, str, str, str, str, str, int, str], dict[str, dict[str, Any]]]] = [
        (key, group)
        for key, group in sorted(groups.items(), key=lambda item: item[0])
        if group.get("start") is not None and group.get("end") is not None
    ]
    for key, group in sorted(groups.items(), key=lambda item: item[0]):
        start = group.get("start")
        end = group.get("end")
        if start is None or end is None:
            unpaired += 1
            continue
        paired += 1
        start_state = derived_hicache_state_from_snapshot(start.get("state_snapshot", {}))
        end_state = derived_hicache_state_from_snapshot(end.get("state_snapshot", {}))
        delta_result = delta_rows_for_event_key(key, start_state, end_state, active_state_keys)
        rows = delta_result["rows"]
        inclusive_rows.extend(rows)
        ignored_state_keys.update(delta_result["ignored_state_keys"])
        if event_has_nested_snapshots(key, group, paired_groups):
            if rows:
                nested_event_count += 1
                nested_transition_count += len(rows)
            continue
        exclusive_rows.extend(rows)
    return {
        "inclusive_rows": inclusive_rows,
        "exclusive_rows": exclusive_rows,
        "paired_event_count": paired,
        "unpaired_snapshot_count": unpaired,
        "nested_event_count": nested_event_count,
        "nested_transition_count": nested_transition_count,
        "ignored_state_keys": sorted(ignored_state_keys),
    }


def event_has_nested_snapshots(
    key: tuple[str, str, str, str, str, str, int, str],
    group: dict[str, dict[str, Any]],
    paired_groups: list[tuple[tuple[str, str, str, str, str, str, int, str], dict[str, dict[str, Any]]]],
) -> bool:
    """判断某个 start/end snapshot 包围区间内是否存在其他 HiCache snapshot。"""

    start = group.get("start")
    end = group.get("end")
    if start is None or end is None:
        return False
    start_ts = int(optional_float(start.get("ts")) or 0)
    end_ts = int(optional_float(end.get("ts")) or start_ts)
    end_dur = int(optional_float(end.get("dur")) or 0)
    start_order = int(start.get("order") or 0)
    end_order = int(end.get("order") or start_order)

    interval_start = min(start_ts, end_ts)
    interval_end = max(start_ts, end_ts + end_dur)
    order_start = min(start_order, end_order)
    order_end = max(start_order, end_order)
    if interval_end <= interval_start and order_end <= order_start:
        return False
    trace_path, pid, tid, *_ = key
    for other_key, other_group in paired_groups:
        if other_key == key:
            continue
        other_trace_path, other_pid, other_tid, *_ = other_key
        if (other_trace_path, other_pid, other_tid) != (trace_path, pid, tid):
            continue
        other_start = other_group.get("start")
        other_end = other_group.get("end")
        if other_start is None or other_end is None:
            continue
        other_start_ts = int(optional_float(other_start.get("ts")) or 0)
        other_end_ts = int(optional_float(other_end.get("ts")) or other_start_ts)
        other_start_order = int(other_start.get("order") or 0)
        other_end_order = int(other_end.get("order") or other_start_order)
        if interval_end > interval_start:
            other_interval_start = min(other_start_ts, other_end_ts)
            if interval_start < other_interval_start < interval_end:
                return True
        if order_end > order_start and (
            order_start < other_start_order < order_end or order_start < other_end_order < order_end
        ):
            return True
    return False


def snapshot_event_key(row: dict[str, Any], source_name: str) -> tuple[str, str, str, str, str, str, int, str]:
    """生成 start/end snapshot 配对使用的事件身份。"""

    return (
        str(row.get("trace_path") or ""),
        str(row.get("pid") or ""),
        str(row.get("tid") or ""),
        str(row.get("target_id") or ""),
        str(row.get("request_id") or ""),
        str(row.get("operation_id") or ""),
        int(optional_float(row.get("ts")) or 0),
        event_base_name(source_name),
    )


def delta_rows_for_event_key(
    key: tuple[str, str, str, str, str, str, int, str],
    start_state: dict[str, Any],
    end_state: dict[str, Any],
    active_state_keys: set[str],
) -> dict[str, Any]:
    """把两个集合状态之间的差分展开成 transition rows。"""

    trace_path, pid, tid, target_id, request_id, operation_id, ts, base_name = key
    rows: list[dict[str, Any]] = []
    ignored_state_keys: set[str] = set()
    for state_key, (add_kind, remove_kind) in DELTA_KIND_BY_STATE_KEY.items():
        start_set = set(str(item) for item in start_state.get(state_key, []) if item is not None)
        end_set = set(str(item) for item in end_state.get(state_key, []) if item is not None)
        if state_key not in active_state_keys:
            if start_set != end_set:
                ignored_state_keys.add(state_key)
            continue
        for kind, pages in ((add_kind, sorted(end_set - start_set)), (remove_kind, sorted(start_set - end_set))):
            if not pages:
                continue
            rows.append(
                {
                    "event_key": event_delta_key(pid, ts, base_name),
                    "trace_path": trace_path,
                    "cache_scope": pid,
                    "tid": tid,
                    "target_id": target_id,
                    "request_id": request_id,
                    "operation_id": operation_id,
                    "ts": ts,
                    "event_base_name": base_name,
                    "transition_kind": kind,
                    "pages": pages,
                }
            )
    return {"rows": rows, "ignored_state_keys": sorted(ignored_state_keys)}


def build_predicted_event_deltas(
    predicted_records: list[dict[str, Any]], active_state_keys: set[str] | None = None
) -> dict[str, Any]:
    """把 C++ transition 明细规整成事件级可比较 delta rows。"""

    state_keys = active_state_keys if active_state_keys is not None else set(DELTA_KIND_BY_STATE_KEY)
    comparable_kinds = {
        kind for state_key, kinds in DELTA_KIND_BY_STATE_KEY.items() if state_key in state_keys for kind in kinds
    }
    grouped: dict[tuple[str, str], set[str]] = {}
    metadata: dict[tuple[str, str], dict[str, Any]] = {}
    for record in predicted_records:
        kind = str(record.get("transition_kind") or "")
        if kind not in comparable_kinds:
            continue
        pages = [str(page) for page in page_set_from_predicted_record(record) if page is not None]
        if not pages:
            continue
        cache_scope = str(record.get("cache_scope") or "")
        ts = int(optional_float(record.get("ts")) or 0)
        base_name = str(record.get("event_base_name") or event_base_name(str(record.get("source_event_name") or "")))
        key = (event_delta_key(cache_scope, ts, base_name), kind)
        grouped.setdefault(key, set()).update(pages)
        metadata.setdefault(
            key,
            {
                "event_key": key[0],
                "cache_scope": cache_scope,
                "ts": ts,
                "event_base_name": base_name,
                "transition_kind": kind,
            },
        )
    rows = []
    for key, pages in sorted(grouped.items()):
        row = dict(metadata[key])
        row["pages"] = sorted(pages)
        rows.append(row)
    return {"rows": rows}


def event_delta_key(cache_scope: str, ts: int, base_name: str) -> str:
    """生成 prediction/oracle 事件级 delta 的紧凑匹配键。"""

    return f"{cache_scope}:{ts}:{base_name}"


def compare_event_delta_rows(
    predicted_rows: list[dict[str, Any]],
    oracle_rows: list[dict[str, Any]],
    allowed_event_keys: set[str] | None = None,
) -> list[dict[str, Any]]:
    """按事件键和 transition kind 比较 predicted/oracle 页集合。"""

    predicted_by_key = {
        (str(row.get("event_key") or ""), str(row.get("transition_kind") or "")): row for row in predicted_rows
    }
    oracle_by_key = {
        (str(row.get("event_key") or ""), str(row.get("transition_kind") or "")): row for row in oracle_rows
    }
    mismatches: list[dict[str, Any]] = []
    for key in sorted(set(predicted_by_key) | set(oracle_by_key)):
        if allowed_event_keys is not None and key[0] not in allowed_event_keys:
            continue
        predicted = predicted_by_key.get(key)
        oracle = oracle_by_key.get(key)
        predicted_pages = set(str(page) for page in (predicted or {}).get("pages", []) if page is not None)
        oracle_pages = set(str(page) for page in (oracle or {}).get("pages", []) if page is not None)
        missing = sorted(oracle_pages - predicted_pages)
        extra = sorted(predicted_pages - oracle_pages)
        if missing or extra:
            event_key, kind = key
            mismatches.append(
                {
                    "event_key": event_key,
                    "transition_kind": kind,
                    "missing_in_predicted": missing,
                    "extra_in_predicted": extra,
                    "predicted_count": len(predicted_pages),
                    "oracle_count": len(oracle_pages),
                }
            )
    return mismatches


def compare_delta_multisets(
    predicted_rows: list[dict[str, Any]], oracle_rows: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    """按 `(transition_kind, page)` multiset 比较 timeline delta。"""

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


def mismatch_value_count(value: Any) -> int:
    """把 mismatch 字段里的 list/count 统一折算成数量。"""

    if isinstance(value, list):
        return len([item for item in value if item is not None])
    try:
        return int(value or 0)
    except (TypeError, ValueError):
        return 0


def summarize_delta_mismatches_by_kind(mismatches: list[dict[str, Any]]) -> dict[str, dict[str, int]]:
    """按 transition kind 汇总 delta mismatch，避免只看 top rows 时漏掉主因。"""

    summary: dict[str, dict[str, int]] = {}
    for row in mismatches:
        kind = str(row.get("transition_kind") or "")
        if not kind:
            continue
        item = summary.setdefault(
            kind,
            {
                "mismatch_rows": 0,
                "missing_in_predicted": 0,
                "extra_in_predicted": 0,
                "predicted_count": 0,
                "oracle_count": 0,
            },
        )
        item["mismatch_rows"] += 1
        item["missing_in_predicted"] += mismatch_value_count(row.get("missing_in_predicted"))
        item["extra_in_predicted"] += mismatch_value_count(row.get("extra_in_predicted"))
        item["predicted_count"] += mismatch_value_count(row.get("predicted_count"))
        item["oracle_count"] += mismatch_value_count(row.get("oracle_count"))
    return {kind: summary[kind] for kind in sorted(summary)}


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
