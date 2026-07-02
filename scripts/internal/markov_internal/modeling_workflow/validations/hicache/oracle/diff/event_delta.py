"""HiCache state snapshot 的事件级 delta oracle。"""

from __future__ import annotations

from typing import Any

from .multiset import count_rows_by_transition_kind, summarize_delta_mismatches_by_kind
from .delta_constants import DELTA_KIND_BY_STATE_KEY
from ..snapshot.records import page_set_from_predicted_record
from ..snapshot.state import (
    derived_hicache_state_from_snapshot,
    event_base_name,
    event_phase,
    optional_float,
    snapshot_sort_key,
)


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
        "mismatch_totals_by_kind": summarize_delta_mismatches_by_kind(
            mismatches,
            missing_key="missing_in_predicted",
            extra_key="extra_in_predicted",
            predicted_key="predicted_count",
            oracle_key="oracle_count",
        ),
        "top_mismatches": mismatches[:20],
        "note": "Exact event delta comparison is intended for self-config prediction; cross-config prediction should use final-state and policy oracle fields.",
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
    paired_groups = [
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
        delta_result = delta_rows_for_event_key(
            key,
            derived_hicache_state_from_snapshot(start.get("state_snapshot", {})),
            derived_hicache_state_from_snapshot(end.get("state_snapshot", {})),
            active_state_keys,
        )
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
        if nested_group_intersects(other_group, interval_start, interval_end, order_start, order_end):
            return True
    return False


def nested_group_intersects(
    other_group: dict[str, dict[str, Any]], interval_start: int, interval_end: int, order_start: int, order_end: int
) -> bool:
    other_start = other_group.get("start")
    other_end = other_group.get("end")
    if other_start is None or other_end is None:
        return False
    other_start_ts = int(optional_float(other_start.get("ts")) or 0)
    other_end_ts = int(optional_float(other_end.get("ts")) or other_start_ts)
    other_start_order = int(other_start.get("order") or 0)
    other_end_order = int(other_end.get("order") or other_start_order)
    if interval_end > interval_start and interval_start < min(other_start_ts, other_end_ts) < interval_end:
        return True
    return order_end > order_start and (
        order_start < other_start_order < order_end or order_start < other_end_order < order_end
    )


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
            if pages:
                rows.append(
                    event_delta_row(
                        kind, pages, trace_path, pid, tid, target_id, request_id, operation_id, ts, base_name
                    )
                )
    return {"rows": rows, "ignored_state_keys": sorted(ignored_state_keys)}


def event_delta_row(
    kind: str,
    pages: list[str],
    trace_path: str,
    pid: str,
    tid: str,
    target_id: str,
    request_id: str,
    operation_id: str,
    ts: int,
    base_name: str,
) -> dict[str, Any]:
    return {
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
