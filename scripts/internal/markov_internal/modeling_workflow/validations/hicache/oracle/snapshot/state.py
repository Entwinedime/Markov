"""HiCache state snapshot 抽取与 final-state 辅助工具。"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from markov_internal.common.trace import load_chrome_trace_events
from ...core.facts import parse_fact_or_none


def optional_float(value: Any) -> float | None:
    """严格解析数字候选；bool 和非法值返回 None。"""

    if value is None or isinstance(value, bool):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def extract_hicache_state_snapshots(trace_paths: list[Path]) -> list[dict[str, Any]]:
    """从 oracle trace 中提取 validation-only HiCache state snapshot。"""

    snapshots: list[dict[str, Any]] = []
    for path in trace_paths:
        if not path.is_file():
            continue
        events, _status = load_chrome_trace_events(path, auto_repair=True)
        for event in events:
            args = event.get("args") if isinstance(event.get("args"), dict) else {}
            if not isinstance(args, dict):
                continue
            fact = parse_fact_or_none(args)
            if fact is None or fact.fact_class != "oracle_state" or fact.role != "state_snapshot":
                continue
            snapshot = args.get("state_snapshot")
            if isinstance(snapshot, dict):
                snapshots.append(
                    {
                        # C++ state model 当前在一个 DagGraph 内聚合所有进程事件；oracle 也必须先按进程取最终快照，再做集合 union。
                        "order": len(snapshots),
                        "trace_path": str(path),
                        "pid": event.get("pid"),
                        "tid": event.get("tid"),
                        "event_name": event.get("name"),
                        "source_event_name": args.get("source_event_name"),
                        "target_id": args.get("target_id"),
                        "request_id": args.get("request_id"),
                        "operation_id": args.get("operation_id"),
                        "ts": event.get("ts"),
                        "dur": event.get("dur"),
                        "object_id": snapshot.get("object_id"),
                        "state_snapshot": snapshot,
                    }
                )
    return snapshots


def latest_derived_state(snapshots: list[dict[str, Any]]) -> dict[str, Any]:
    """按进程取最后一个 completed snapshot，并合并成 oracle final state。"""

    latest_by_process: dict[tuple[str, str], dict[str, Any]] = {}
    completed_snapshots = [row for row in snapshots if snapshot_is_completed_state(row)]
    source_snapshots = completed_snapshots if completed_snapshots else snapshots
    for row in source_snapshots:
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
            continue
        if not snapshot_is_hiradix_cache_state(row, snapshot):
            continue
        derived = derived_hicache_state_from_snapshot(snapshot)
        if isinstance(derived, dict) and any(isinstance(derived.get(key), list) for key in derived):
            key = (str(row.get("trace_path") or ""), str(row.get("pid") or ""))
            current = latest_by_process.get(key)
            if current is None or snapshot_sort_key(row) >= snapshot_sort_key(current):
                latest_by_process[key] = row

    union: dict[str, list[str]] = {}
    for row in latest_by_process.values():
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict):
            continue
        if not snapshot_is_hiradix_cache_state(row, snapshot):
            continue
        derived = derived_hicache_state_from_snapshot(snapshot)
        if not isinstance(derived, dict):
            continue
        for key, value in derived.items():
            if not isinstance(value, list):
                continue
            current = union.setdefault(key, [])
            seen = set(current)
            for item in value:
                page = str(item)
                if page not in seen:
                    current.append(page)
                    seen.add(page)
    return {key: sorted(value) for key, value in union.items()}


def derived_hicache_state_from_snapshot(snapshot: dict[str, Any]) -> dict[str, Any]:
    """从 state snapshot 原始节点重新派生集合状态。"""

    nodes = snapshot.get("nodes")
    if not isinstance(nodes, list):
        return {}

    result: dict[str, set[str]] = {
        "l1_resident_pages": set(),
        "l2_resident_pages": set(),
        "dirty_pages": set(),
        "backuped_pages": set(),
        "evicted_pages": set(),
        "locked_pages": set(),
    }
    for node in nodes:
        if not isinstance(node, dict):
            continue
        pages = page_keys_from_snapshot_hash(node.get("hash_value"))
        has_device_value = bool(node.get("has_device_value"))
        has_host_value = bool(node.get("has_host_value"))
        backuped = bool(node.get("backuped")) or has_host_value
        evicted = bool(node.get("evicted"))
        if has_device_value:
            result["l1_resident_pages"].update(pages)
        if has_host_value:
            result["l2_resident_pages"].update(pages)
        # SGLang HiRadixCache 目前没有可靠暴露 dirty 字段；write-back 下 device-resident 且未备份到 host 的页就是 state model 需要维护的 dirty 页。
        if node.get("dirty") or (has_device_value and not backuped and not evicted):
            result["dirty_pages"].update(pages)
        if backuped:
            result["backuped_pages"].update(pages)
        if evicted:
            result["evicted_pages"].update(pages)
        if (
            int(optional_float(node.get("lock_ref")) or 0) > 0
            or int(optional_float(node.get("host_ref_counter")) or 0) > 0
        ):
            result["locked_pages"].update(pages)
    return {key: sorted(value) for key, value in result.items()}


def page_keys_from_snapshot_hash(value: Any) -> list[str]:
    """从 snapshot hash_value 字段提取 page key 列表。"""

    if value is None:
        return []
    if isinstance(value, list):
        return [str(item) for item in value if item is not None]
    return [str(value)]


def normalize_hicache_state_for_oracle_compare(state: dict[str, Any], page_key_mode: str) -> dict[str, Any]:
    """按 page_key_mode 归一化 state 中的集合字段。"""

    normalized: dict[str, Any] = {}
    for key, value in state.items():
        if isinstance(value, list):
            normalized[key] = sorted(
                {normalize_hicache_page_key(item, page_key_mode) for item in value if item is not None}
            )
        else:
            normalized[key] = value
    return normalized


def normalize_hicache_page_key(value: Any, page_key_mode: str) -> str:
    """归一化单个 page key；strip_scope 模式会去掉 scope 前缀。"""

    page = str(value)
    if page_key_mode == "strip_scope" and "|" in page:
        return page.split("|", 1)[1]
    return page


def snapshot_sort_key(row: dict[str, Any]) -> tuple[int, int, int]:
    """state snapshot 的逻辑顺序。

    Python probe 的 start/end 事件在 merged trace 中可能同 timestamp 且顺序反转。
    同一时刻优先使用 end 快照，它更接近一次调用完成后的真实状态。
    """

    ts = int(optional_float(row.get("ts")) or 0)
    source_name = str(row.get("source_event_name") or row.get("event_name") or "")
    phase_score = 1 if source_name.endswith("_end") else 0
    order = int(row.get("order") or 0)
    return (ts, phase_score, order)


def diff_hicache_sets(model_final: dict[str, Any], oracle_final: dict[str, Any]) -> dict[str, Any]:
    """比较 model final state 和 oracle final state 的集合字段。"""

    keys = [
        "l1_resident_pages",
        "l2_resident_pages",
        "l3_resident_pages",
        "dirty_pages",
        "backuped_pages",
        "evicted_pages",
        "locked_pages",
        "prefetch_planned_pages",
        "prefetch_ready_pages",
        "prefetch_late_pages",
        "prefetch_suppressed_pages",
    ]
    diff: dict[str, Any] = {}
    for key in keys:
        if key not in oracle_final:
            continue
        model_set = set(str(item) for item in model_final.get(key, []) if item is not None)
        oracle_set = set(str(item) for item in oracle_final.get(key, []) if item is not None)
        missing = sorted(oracle_set - model_set)
        extra = sorted(model_set - oracle_set)
        diff[key] = {
            "match": not missing and not extra,
            "missing_in_model": missing,
            "extra_in_model": extra,
            "model_count": len(model_set),
            "oracle_count": len(oracle_set),
        }
    return diff


def final_state_counts(state: dict[str, Any]) -> dict[str, int]:
    """统计 final state 中所有集合字段的大小，帮助暴露未参与 diff 的状态。"""

    counts: dict[str, int] = {}
    for key, value in sorted(state.items()):
        if isinstance(value, list):
            counts[key] = len({str(item) for item in value if item is not None})
    return counts


def unchecked_model_state_keys(model_final: dict[str, Any], oracle_final: dict[str, Any]) -> list[str]:
    """列出 model 有、但 oracle snapshot 没有的集合字段。

    这些字段不会参与 `final_state_match`，但必须在文档和后续 probe 设计中显式处理。
    """

    keys: list[str] = []
    for key, value in sorted(model_final.items()):
        if isinstance(value, list) and key not in oracle_final and any(item is not None for item in value):
            keys.append(key)
    return keys


def union_hicache_states(states: Any) -> dict[str, list[str]]:
    """把多个 cache object 的集合状态合并成一次全局可见状态。"""

    union: dict[str, set[str]] = {}
    for state in states:
        if not isinstance(state, dict):
            continue
        for key, value in state.items():
            if not isinstance(value, list):
                continue
            target = union.setdefault(str(key), set())
            target.update(str(item) for item in value if item is not None)
    return {key: sorted(value) for key, value in union.items()}


def snapshot_object_id_prefix(row: dict[str, Any], snapshot: dict[str, Any]) -> str:
    """返回 snapshot object_id 中类似 class name 的前缀。"""

    object_id = str(row.get("object_id") or snapshot.get("object_id") or "")
    return object_id.split(":", 1)[0] if object_id else "unknown"


def snapshot_is_hiradix_cache_state(row: dict[str, Any], snapshot: dict[str, Any]) -> bool:
    """判断 snapshot 是否来自 HiCache state model 要验证的 HiRadixCache 对象。"""

    return snapshot_object_id_prefix(row, snapshot) == "HiRadixCache"


def snapshot_is_completed_state(row: dict[str, Any]) -> bool:
    """判断 state snapshot 是否代表一次调用完成后的状态。

    Python probe 会同时输出 start/end 包围快照。start 快照描述调用前状态，
    如果把它当作最终状态，trace 尾部缺少对应 end snapshot 时会把尚未释放的
    lock/ref 误判为最终 cache state。因此 final oracle 和 timeline oracle 只
    使用 end 或无 phase 的快照；完全没有 completed snapshot 时 final oracle
    才由调用方 fallback 到原始快照集合。
    """

    source_name = str(row.get("source_event_name") or row.get("event_name") or "")
    return event_phase(source_name) != "start"


def snapshot_logical_time_us(row: dict[str, Any]) -> int:
    """返回 snapshot 参与 timeline 排序的逻辑时间。

    end snapshot 的真实状态变化点在 duration 末尾，因此使用 `ts + dur`。
    """

    ts = int(optional_float(row.get("ts")) or 0)
    source_name = str(row.get("source_event_name") or row.get("event_name") or "")
    if event_phase(source_name) == "end":
        ts += int(optional_float(row.get("dur")) or 0)
    return ts


def snapshot_timeline_sort_key(row: dict[str, Any]) -> tuple[int, int, int]:
    """生成稳定 timeline 排序键，确保同时间戳下 start 先于 end。"""

    source_name = str(row.get("source_event_name") or row.get("event_name") or "")
    phase_score = 0 if event_phase(source_name) == "start" else 1
    return (snapshot_logical_time_us(row), phase_score, int(row.get("order") or 0))


def event_phase(name: str) -> str:
    """从 probe event name 后缀解析 start/end phase。"""

    clean = name.split(":", 1)[0]
    if clean.endswith("_start"):
        return "start"
    if clean.endswith("_end"):
        return "end"
    return ""


def event_base_name(name: str) -> str:
    """去掉 start/end 后缀，得到 prediction 和 oracle 共享的事件基名。"""

    clean = name.split(":", 1)[0]
    if clean.endswith("_start"):
        return clean[: -len("_start")]
    if clean.endswith("_end"):
        return clean[: -len("_end")]
    return clean
