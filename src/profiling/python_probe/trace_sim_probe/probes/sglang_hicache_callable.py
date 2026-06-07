"""SGLang HiCache callable probe。

该 probe 复用 `generic_callable` 的函数包装逻辑，只补充 HiCache 建模需要的
`page_hashes:` 字段 source。这样通用 probe 不再包含 HiCache 特化规则。
"""

from __future__ import annotations

import hashlib
import os
from typing import Any

from trace_sim_probe.probes import generic_callable as _base


def _truthy(value: str | None) -> bool:
    return value is not None and value.lower() not in ("", "0", "false", "no", "off")


def _page_hashes_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("page_hashes:"):
        return (False, False, None)
    found, value = _extract_page_hashes(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, value)


def _page_hashes_concat_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("page_hashes_concat:"):
        return (False, False, None)
    found, value = _extract_page_hashes_concat(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, value)


def _page_hashes_after_prefix_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("page_hashes_after_prefix:"):
        return (False, False, None)
    found, value = _extract_page_hashes_after_prefix(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, value)


def _hicache_state_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if source != "hicache_state:self":
        return (False, False, None)
    if not _truthy(os.environ.get("TRACE_SIM_HICACHE_STATE_TRACE")):
        return (True, True, _base.ExtractedField({"enabled": False}, model_input=False, event_kind="state_snapshot"))
    if not args:
        return (True, False, None)
    snapshot = _snapshot_hicache_object(args[0])
    return (True, True, _base.ExtractedField(snapshot, model_input=False, event_kind="state_snapshot"))


def _prefetch_progress_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("prefetch_progress:"):
        return (False, False, None)
    found, value = _extract_prefetch_progress(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, value)


def _extract_page_hashes(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """按 SGLang HiCache page hash 规则采集 page identity。

    表达式格式为 `page_hashes:<tokens>,<page_size>[,<prior_hash>]`。
    `<tokens>` 可以是 RadixKey，也可以是 token id 序列；`<prior_hash>` 用于
    prefetch 这类从已有前缀继续计算 page hash 的场景。
    """

    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if len(parts) < 2:
        return (False, None)
    found_tokens, tokens = _base._extract_raw_value(parts[0], "page_identity", bound, args, kwargs, result)
    found_page_size, page_size_value = _base._extract_raw_value(parts[1], "page_size", bound, args, kwargs, result)
    if not found_page_size:
        page_size_value = parts[1]
        found_page_size = True
    if not found_tokens or not found_page_size:
        return (False, None)
    page_size = _safe_int(page_size_value)
    if page_size is None or page_size <= 0:
        return (False, None)
    prior_hash = None
    if len(parts) >= 3:
        found_prior, prior_value = _base._extract_raw_value(parts[2], "prior_hash", bound, args, kwargs, result)
        if found_prior and prior_value:
            prior_hash = str(prior_value)
    return (True, _compute_page_hashes(tokens, page_size, prior_hash))


def _extract_page_hashes_concat(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """把两段 token 序列拼接后按 HiCache page hash 规则采集 page identity。

    表达式格式为 `page_hashes_concat:<prefix_tokens>,<tokens>,<page_size>[,<prior_hash>]`。
    该 source 用于 page size what-if 下的 prefetch：此时 base run 的 `last_hash`
    属于 base page size，不能作为 target page size 的 parent hash；必须用完整
    prefix token path 重新计算 target page hashes。
    """

    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if len(parts) < 3:
        return (False, None)
    found_prefix, prefix_tokens = _base._extract_raw_value(parts[0], "prefix_tokens", bound, args, kwargs, result)
    found_tokens, tokens = _base._extract_raw_value(parts[1], "tokens", bound, args, kwargs, result)
    found_page_size, page_size_value = _base._extract_raw_value(parts[2], "page_size", bound, args, kwargs, result)
    if not found_page_size:
        page_size_value = parts[2]
        found_page_size = True
    if not found_prefix or not found_tokens or not found_page_size:
        return (False, None)
    page_size = _safe_int(page_size_value)
    if page_size is None or page_size <= 0:
        return (False, None)
    prior_hash = None
    if len(parts) >= 4:
        found_prior, prior_value = _base._extract_raw_value(parts[3], "prior_hash", bound, args, kwargs, result)
        if found_prior and prior_value:
            prior_hash = str(prior_value)
    return (True, _compute_page_hashes(_tokens_for_page_hash(prefix_tokens) + _tokens_for_page_hash(tokens), page_size, prior_hash))


def _extract_page_hashes_after_prefix(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """按完整 prefix path 计算 parent hash，只返回 suffix token 产生的 pages。

    表达式格式为
    `page_hashes_after_prefix:<prefix_tokens>,<tokens>,<page_size>[,<prior_hash>]`。
    该 source 专门服务 prefetch target identity：`prefix_tokens` 用于在目标
    page size 下重新得到 parent hash，输出只保留 `<tokens>` 对应的完整 pages，
    避免把已经命中的 prefix pages 混入 prefetch planned set。
    """

    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if len(parts) < 3:
        return (False, None)
    found_prefix, prefix_tokens = _base._extract_raw_value(parts[0], "prefix_tokens", bound, args, kwargs, result)
    found_tokens, tokens = _base._extract_raw_value(parts[1], "tokens", bound, args, kwargs, result)
    found_page_size, page_size_value = _base._extract_raw_value(parts[2], "page_size", bound, args, kwargs, result)
    if not found_page_size:
        page_size_value = parts[2]
        found_page_size = True
    if not found_prefix or not found_tokens or not found_page_size:
        return (False, None)
    page_size = _safe_int(page_size_value)
    if page_size is None or page_size <= 0:
        return (False, None)
    prior_hash = None
    if len(parts) >= 4:
        found_prior, prior_value = _base._extract_raw_value(parts[3], "prior_hash", bound, args, kwargs, result)
        if found_prior and prior_value:
            prior_hash = str(prior_value)
    suffix_page_count = len(_tokens_for_page_hash(tokens)) // page_size
    if suffix_page_count <= 0:
        return (True, [])
    full_hashes = _compute_page_hashes(_tokens_for_page_hash(prefix_tokens) + _tokens_for_page_hash(tokens), page_size, prior_hash)
    return (True, full_hashes[-suffix_page_count:])


def _extract_prefetch_progress(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """读取一次 prefetch progress 的只读证据。

    表达式格式为 `prefetch_progress:<cache_obj>,<req_id>`。该 source 不调用
    SGLang 的 termination 方法，只读取已有 operation 状态，用于后续验证
    planned/ready/late/suppressed page 推导。
    """

    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if len(parts) < 2:
        return (False, None)
    found_obj, obj = _base._extract_raw_value(parts[0], field_name="cache_obj", bound=bound, args=args, kwargs=kwargs, result=result)
    found_req, req_id_value = _base._extract_raw_value(parts[1], field_name="request_id", bound=bound, args=args, kwargs=kwargs, result=result)
    if not found_obj:
        return (False, None)
    req_id = str(req_id_value) if found_req and req_id_value is not None else ""
    page_size = _safe_int(getattr(obj, "page_size", None)) or 0
    ongoing = getattr(obj, "ongoing_prefetch", {}) or {}
    loaded_by_reqid = getattr(obj, "prefetch_loaded_tokens_by_reqid", {}) or {}
    row: dict[str, Any] = {
        "request_id": req_id,
        "policy": _jsonable_compact(getattr(obj, "prefetch_stop_policy", "")),
        "page_size": page_size,
        "ongoing_prefetch_count": _base._safe_len(ongoing) or 0,
        "has_ongoing_prefetch": req_id in ongoing if req_id else False,
        "loaded_tokens_evidence": _safe_int(loaded_by_reqid.get(req_id)) if isinstance(loaded_by_reqid, dict) and req_id else None,
        "check_return": bool(result) if result is not None else None,
    }
    if not req_id or req_id not in ongoing:
        return (True, row)

    info = ongoing.get(req_id)
    if not isinstance(info, tuple) or len(info) < 4:
        return (True, row)
    _last_host_node, prefetch_key, host_indices, operation = info[:4]
    operation_hash_value = _jsonable_compact(getattr(operation, "hash_value", None))
    completed_tokens = _safe_int(getattr(operation, "completed_tokens", None)) or 0
    prefetch_tokens = _base._safe_len(prefetch_key) or 0
    host_tokens = _base._safe_len(host_indices) or 0
    row.update(
        {
            "prefetch_tokens": prefetch_tokens,
            "host_tokens": host_tokens,
            "operation_hash_pages": operation_hash_value,
            "operation_hash_page_count": _base._safe_len(getattr(operation, "hash_value", None)) or 0,
            "completed_tokens": completed_tokens,
            "ready_pages_estimate": completed_tokens // page_size if page_size > 0 else 0,
            "late_tokens_estimate": max(prefetch_tokens - completed_tokens, 0),
        }
    )
    is_terminated = getattr(operation, "is_terminated", None)
    if callable(is_terminated):
        try:
            row["operation_terminated"] = bool(is_terminated())
        except Exception:
            row["operation_terminated"] = None
    return (True, row)


def _safe_int(value: Any) -> int | None:
    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _compute_page_hashes(value: Any, page_size: int, prior_hash: str | None) -> list[str]:
    """复用 RadixKey.hash_page；普通 token 序列按同样 SHA256 规则计算。"""

    if hasattr(value, "page_aligned") and hasattr(value, "hash_page"):
        key = value.page_aligned(page_size)
        hashes: list[str] = []
        parent_hash = prior_hash
        for start in range(0, len(key), page_size):
            end = min(start + page_size, len(key))
            if end <= start:
                continue
            parent_hash = key.hash_page(start, end, parent_hash)
            hashes.append(parent_hash)
        return hashes

    tokens = _tokens_for_page_hash(value)
    aligned_len = len(tokens) // page_size * page_size
    hashes = []
    parent_hash = prior_hash
    for start in range(0, aligned_len, page_size):
        parent_hash = _hash_token_page(tokens[start : start + page_size], parent_hash)
        hashes.append(parent_hash)
    return hashes


def _tokens_for_page_hash(value: Any) -> list[Any]:
    """为 page hash 读取完整 token 序列。

    通用 probe 的 `_safe_list` 会截断到 64 个元素，适合作摘要，但 page hash
    需要按 page_size 对齐计算，不能在这里截断。
    """

    if value is None:
        return []
    if hasattr(value, "tolist") and callable(value.tolist):
        value = value.tolist()
    elif hasattr(value, "token_ids"):
        value = getattr(value, "token_ids")
    try:
        return list(value)
    except TypeError:
        return []


def _hash_token_page(tokens: list[Any], prior_hash: str | None) -> str:
    hasher = hashlib.sha256()
    if prior_hash:
        hasher.update(bytes.fromhex(prior_hash))
    for token in tokens:
        if isinstance(token, (list, tuple)):
            for item in token:
                hasher.update(int(item).to_bytes(4, byteorder="little", signed=False))
        else:
            hasher.update(int(token).to_bytes(4, byteorder="little", signed=False))
    return hasher.hexdigest()


def _snapshot_hicache_object(obj: Any) -> dict[str, Any]:
    """读取 SGLang HiCache 对象的轻量状态快照。

    该函数只在显式验证模式下执行。实现使用宽松 introspection，是为了同时适配
    HiRadixCache、HiCacheController 和 fixture 中的最小假对象；缺失字段会记录为空，
    不影响业务执行。
    """

    nodes = _collect_radix_nodes(obj)
    derived = _derive_page_sets(nodes)
    return {
        "enabled": True,
        "object_type": type(obj).__name__,
        "object_id": f"{type(obj).__name__}:{id(obj)}",
        "page_size": _safe_int(getattr(obj, "page_size", None)),
        "nodes": nodes,
        "derived": derived,
        "capacity": _snapshot_capacity(obj),
        "controller": _snapshot_controller(obj),
    }


def _collect_radix_nodes(obj: Any) -> list[dict[str, Any]]:
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
    children = []
    for attr in ("children", "childs", "child_nodes"):
        value = getattr(node, attr, None)
        if isinstance(value, dict):
            children.extend(value.values())
        elif isinstance(value, (list, tuple, set)):
            children.extend(value)
    return [item for item in children if item is not None]


def _snapshot_node(node: Any) -> dict[str, Any]:
    parent = getattr(node, "parent", None)
    hash_value = _jsonable_compact(getattr(node, "hash_value", None))
    key_value = getattr(node, "key", getattr(node, "token_ids", None))
    device_value = _first_attr(node, ("value", "device_value", "device_indices"))
    host_value = _first_attr(node, ("host_value", "host_indices"))
    return {
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


def _snapshot_controller(obj: Any) -> dict[str, Any]:
    controller = _first_attr(obj, ("cache_controller", "controller")) or obj
    queues = {}
    for attr in ("load_queue", "write_queue", "prefetch_queue", "backup_queue", "ack_queue"):
        queues[attr] = _base._safe_len(getattr(controller, attr, None)) or 0
    ongoing = _first_attr(controller, ("ongoing_operation", "current_operation", "operation"))
    storage = _first_attr(controller, ("storage", "storage_backend", "file_backend"))
    return {
        "queues": queues,
        "ongoing_operation_id": _jsonable_compact(_first_attr(ongoing, ("id", "operation_id")) if ongoing is not None else None),
        "ongoing_hash_pages": _jsonable_compact(_first_attr(ongoing, ("hash_value", "hash_pages")) if ongoing is not None else None),
        "completed_tokens": _safe_int(_first_attr(ongoing, ("completed_tokens",))) if ongoing is not None else None,
        "prefetch_occupied_tokens": _safe_int(getattr(controller, "prefetch_occupied_tokens", None)),
        "storage_backend_enabled": storage is not None,
        "storage_backend_type": type(storage).__name__ if storage is not None else "",
    }


def _snapshot_capacity(obj: Any) -> dict[str, Any]:
    """采集 HiCache 容量和策略事实。

    capacity 信息只进入 validation-only state snapshot。它用于解释 target
    配置下的有效 L1/L2 budget，不能作为业务 DAG 事件消费。
    """

    controller = _first_attr(obj, ("cache_controller", "controller")) or obj
    page_size = _safe_int(_first_attr(obj, ("page_size",))) or _safe_int(_first_attr(controller, ("page_size",)))
    device_pool = (
        _first_attr(controller, ("mem_pool_device", "device_pool"))
        or _first_attr(obj, ("mem_pool_device", "device_pool", "kv_cache", "token_to_kv_pool_device"))
    )
    host_pool = (
        _first_attr(controller, ("mem_pool_host", "host_pool"))
        or _first_attr(obj, ("mem_pool_host", "host_pool", "token_to_kv_pool_host"))
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


def _safe_call_int(obj: Any, method_name: str) -> int | None:
    method = getattr(obj, method_name, None) if obj is not None else None
    if not callable(method):
        return None
    try:
        return _safe_int(method())
    except Exception:
        return None


def _page_count_from_tokens(tokens: int | None, page_size: int | None) -> int | None:
    if tokens is None or page_size is None or page_size <= 0:
        return None
    return tokens // page_size


def _derive_page_sets(nodes: list[dict[str, Any]]) -> dict[str, list[str]]:
    l1: set[str] = set()
    l2: set[str] = set()
    dirty: set[str] = set()
    backuped: set[str] = set()
    locked: set[str] = set()
    evicted: set[str] = set()
    for node in nodes:
        pages = _page_keys_from_hash_value(node.get("hash_value"))
        if node.get("has_device_value"):
            l1.update(pages)
        if node.get("has_host_value"):
            l2.update(pages)
        if node.get("dirty"):
            dirty.update(pages)
        if node.get("backuped"):
            backuped.update(pages)
        if node.get("evicted"):
            evicted.update(pages)
        if (node.get("lock_ref") or 0) > 0 or (node.get("host_ref_counter") or 0) > 0:
            locked.update(pages)
    return {
        "l1_resident_pages": sorted(l1),
        "l2_resident_pages": sorted(l2),
        "dirty_pages": sorted(dirty),
        "backuped_pages": sorted(backuped),
        "locked_pages": sorted(locked),
        "evicted_pages": sorted(evicted),
    }


def _page_keys_from_hash_value(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, list):
        return [str(item) for item in value if item is not None]
    return [str(value)]


def _first_attr(obj: Any, names: tuple[str, ...]) -> Any:
    if obj is None:
        return None
    for name in names:
        if hasattr(obj, name):
            return getattr(obj, name)
    return None


def _jsonable_compact(value: Any) -> Any:
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, (list, tuple)):
        return [_jsonable_compact(item) for item in value[:64]]
    if isinstance(value, dict):
        return {str(key): _jsonable_compact(item) for key, item in list(value.items())[:64]}
    return str(value)


_base.register_source_extractor(_page_hashes_source)
_base.register_source_extractor(_page_hashes_concat_source)
_base.register_source_extractor(_page_hashes_after_prefix_source)
_base.register_source_extractor(_hicache_state_source)
_base.register_source_extractor(_prefetch_progress_source)

install = _base.install
TARGET_MODULES = _base.TARGET_MODULES
