"""HiCache validation snapshot helper。"""

from __future__ import annotations

from typing import Any

from trace_sim_probe.probes import generic_callable as _base

from .common import (
    _first_attr,
    _jsonable_compact,
    _page_count_from_tokens,
    _safe_call_int,
    _safe_int,
)


def _snapshot_hicache_object(obj: Any) -> dict[str, Any]:
    """读取 SGLang HiCache 对象的轻量状态快照。

    该函数只在显式验证模式下执行，只保留 final-state / transition validator
    需要的 radix node state 和 capacity/policy evidence。
    """

    nodes = _collect_radix_nodes(obj)
    return {
        "enabled": True,
        "object_id": f"{type(obj).__name__}:{id(obj)}",
        "page_size": _safe_int(getattr(obj, "page_size", None)),
        "nodes": nodes,
        "capacity": _snapshot_capacity(obj),
    }


def _collect_radix_nodes(obj: Any) -> list[dict[str, Any]]:
    """遍历可能的 radix tree 入口，采集去重后的 node 快照。"""

    candidates = []
    for attr in ("root_node", "root", "tree_root", "node"):
        if hasattr(obj, attr):
            candidates.append(getattr(obj, attr))
    for attr in ("nodes", "node_map"):
        value = getattr(obj, attr, None)
        if isinstance(value, dict):
            candidates.extend(value.values())
        elif isinstance(value, (list, tuple, set)):
            candidates.extend(value)

    result: list[dict[str, Any]] = []
    seen: set[int] = set()
    stack = [item for item in candidates if item is not None]
    while stack and len(result) < 4096:
        node = stack.pop()
        identity = id(node)
        if identity in seen:
            continue
        seen.add(identity)
        row = _snapshot_node(node)
        result.append(row)
        stack.extend(_iter_children(node))
    return result


def _iter_children(node: Any) -> list[Any]:
    """读取当前 SGLang radix node 可能暴露的 children 容器。"""

    children = []
    for attr in ("children", "childs", "child_nodes"):
        value = getattr(node, attr, None)
        if isinstance(value, dict):
            children.extend(value.values())
        elif isinstance(value, (list, tuple, set)):
            children.extend(value)
    return [item for item in children if item is not None]


def _snapshot_node(node: Any) -> dict[str, Any]:
    """采集单个 radix node 的状态字段。"""

    parent = getattr(node, "parent", None)
    hash_value = _jsonable_compact(getattr(node, "hash_value", None))
    key_value = getattr(node, "key", getattr(node, "token_ids", None))
    device_value = _first_attr(node, ("value", "device_value", "device_indices"))
    host_value = _first_attr(node, ("host_value", "host_indices"))
    row = {
        "node_id": _jsonable_compact(_first_attr(node, ("id", "node_id"))),
        "parent_id": _jsonable_compact(_first_attr(parent, ("id", "node_id")) if parent is not None else None),
        "hash_value": hash_value,
        "key_token_length": _base._safe_len(key_value),
        "has_device_value": device_value is not None,
        "has_host_value": host_value is not None,
        "evicted": bool(getattr(node, "evicted", False)),
        "backuped": bool(getattr(node, "backuped", getattr(node, "backed_up", False))),
        "dirty": bool(getattr(node, "dirty", False)),
        "hit_count": _safe_int(getattr(node, "hit_count", 0)) or 0,
        "lock_ref": _safe_int(_first_attr(node, ("lock_ref", "lock_ref_count", "lock_ref_counter"))) or 0,
        "host_ref_counter": _safe_int(getattr(node, "host_ref_counter", 0)) or 0,
        "child_count": len(_iter_children(node)),
    }
    return row


def _snapshot_capacity(obj: Any) -> dict[str, Any]:
    """采集 HiCache 容量和策略事实。

    capacity 信息只进入 validation-only state snapshot。它用于解释 target
    配置下的有效 L1/L2 budget，不能作为业务 DAG 事件消费。
    """

    controller = _first_attr(obj, ("cache_controller", "controller")) or obj
    page_size = _safe_int(_first_attr(obj, ("page_size",))) or _safe_int(_first_attr(controller, ("page_size",)))
    device_pool = _first_attr(controller, ("mem_pool_device", "device_pool")) or _first_attr(
        obj, ("mem_pool_device", "device_pool", "kv_cache", "token_to_kv_pool_device")
    )
    host_pool = _first_attr(controller, ("mem_pool_host", "host_pool")) or _first_attr(
        obj, ("mem_pool_host", "host_pool", "token_to_kv_pool_host")
    )
    l1 = _snapshot_pool_capacity(device_pool, page_size)
    l2 = _snapshot_pool_capacity(host_pool, page_size)
    prefetch_capacity_limit_tokens = _safe_int(_first_attr(controller, ("prefetch_capacity_limit",)))
    prefetch_threshold_tokens = _safe_int(_first_attr(controller, ("prefetch_threshold",)))
    return {
        "page_size": page_size,
        "write_policy": _jsonable_compact(_first_attr(controller, ("write_policy",))),
        "prefetch_policy": _jsonable_compact(_first_attr(obj, ("prefetch_stop_policy", "storage_prefetch_policy"))),
        "write_through_threshold": _safe_int(_first_attr(obj, ("write_through_threshold",))),
        "load_back_threshold": _safe_int(_first_attr(obj, ("load_back_threshold",))),
        "prefetch_threshold_tokens": prefetch_threshold_tokens,
        "prefetch_threshold_pages": _page_count_from_tokens(prefetch_threshold_tokens, page_size),
        "prefetch_capacity_limit_tokens": prefetch_capacity_limit_tokens,
        "prefetch_capacity_limit_pages": _page_count_from_tokens(prefetch_capacity_limit_tokens, page_size),
        "prefetch_tokens_occupied": _safe_int(_first_attr(controller, ("prefetch_tokens_occupied",))),
        "enable_storage": _jsonable_compact(_first_attr(controller, ("enable_storage",))),
        "storage_batch_size": _safe_int(_first_attr(controller, ("storage_batch_size",))),
        "l1_pool": l1,
        "l2_pool": l2,
        "l1_capacity_tokens": l1.get("capacity_tokens"),
        "l1_capacity_pages": l1.get("capacity_pages"),
        "l1_available_tokens": l1.get("available_tokens"),
        "l1_available_pages": l1.get("available_pages"),
        "l2_capacity_tokens": l2.get("capacity_tokens"),
        "l2_capacity_pages": l2.get("capacity_pages"),
        "l2_available_tokens": l2.get("available_tokens"),
        "l2_available_pages": l2.get("available_pages"),
    }


def _snapshot_pool_capacity(pool: Any, page_size: int | None) -> dict[str, Any]:
    """读取 KV pool 容量。

    SGLang host/device pool 的 `size` 通常表示 token slot 数量；按 page_size
    向下取整得到可建模的 page 容量。`page_num` 保留为 raw 字段用于排查
    实现细节，不直接覆盖模型容量。
    """

    capacity_tokens = _safe_int(_first_attr(pool, ("size", "capacity", "num_tokens"))) if pool is not None else None
    available_tokens = _safe_call_int(pool, "available_size") if pool is not None else None
    if available_tokens is None and pool is not None:
        available_tokens = _base._safe_len(_first_attr(pool, ("free_slots", "free_pages")))
    pool_page_size = _safe_int(_first_attr(pool, ("page_size",))) or page_size
    return {
        "pool_type": type(pool).__name__ if pool is not None else "",
        "page_size": pool_page_size,
        "capacity_tokens": capacity_tokens,
        "capacity_pages": _page_count_from_tokens(capacity_tokens, pool_page_size),
        "available_tokens": available_tokens,
        "available_pages": _page_count_from_tokens(available_tokens, pool_page_size),
        "raw_page_num": _safe_int(_first_attr(pool, ("page_num", "num_pages"))) if pool is not None else None,
    }
