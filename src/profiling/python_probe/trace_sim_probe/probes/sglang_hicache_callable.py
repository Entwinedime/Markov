"""SGLang HiCache callable probe。

该 probe 复用 `generic_callable` 的函数包装逻辑，只补充 HiCache 建模需要的
token/range source 和 validation-only state snapshot。这样通用 probe 不包含
HiCache 特化规则，HiCache 新后端也不再依赖按 page size 预声明的 page identity。
"""

from __future__ import annotations

import hashlib
import os
from typing import Any

from trace_sim_probe.probes import generic_callable as _base


_TOKEN_PATHS_EMITTED_BY_SCOPE: dict[str, set[str]] = {}
_HICACHE_SEQUENCE_BY_SCOPE: dict[str, int] = {}
_TOKEN_HASH_ALGO = "sglang_radix_sha256_v1"


def _truthy(value: str | None) -> bool:
    return value is not None and value.lower() not in ("", "0", "false", "no", "off")


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
        return (
            True,
            True,
            _base.ExtractedField(
                {"enabled": False},
                model_input=False,
                event_kind="state_snapshot",
                extra_args={
                    "dag_input": False,
                    "state_model_input": False,
                    "fact_class": "oracle_state",
                },
            ),
        )
    if not args:
        return (True, False, None)
    snapshot = _snapshot_hicache_object(args[0])
    return (
        True,
        True,
        _base.ExtractedField(
            snapshot,
            model_input=False,
            event_kind="state_snapshot",
            extra_args={
                "dag_input": False,
                "state_model_input": False,
                "fact_class": "oracle_state",
            },
        ),
    )


def _token_path_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("token_path:"):
        return (False, False, None)
    found, value = _extract_token_path(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, _base.ExtractedField(value) if found else None)


def _token_span_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("token_span:"):
        return (False, False, None)
    found, value = _extract_token_span(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, value)


def _token_path_concat_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("token_path_concat:"):
        return (False, False, None)
    found, value = _extract_token_path_concat(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, _base.ExtractedField(value) if found else None)


def _token_span_concat_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("token_span_concat:"):
        return (False, False, None)
    found, value = _extract_token_span_concat(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, value)


def _node_token_path_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("node_token_path:"):
        return (False, False, None)
    found, value = _extract_node_token_path(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, _base.ExtractedField(value) if found else None)


def _node_token_span_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("node_token_span:"):
        return (False, False, None)
    found, value = _extract_node_token_span(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, value)


def _node_token_count_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("node_token_count:"):
        return (False, False, None)
    found, node = _extract_source_value(source.split(":", 1)[1], field_name, bound, args, kwargs, result)
    if not found:
        return (True, False, None)
    return (True, True, len(_full_key_tokens(node)))


def _node_token_path_concat_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("node_token_path_concat:"):
        return (False, False, None)
    found, value = _extract_node_token_path_concat(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, _base.ExtractedField(value) if found else None)


def _node_token_span_concat_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("node_token_span_concat:"):
        return (False, False, None)
    found, value = _extract_node_token_span_concat(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, value)


def _hicache_cache_scope_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("hicache_cache_scope:"):
        return (False, False, None)
    found, value = _extract_source_value(source.split(":", 1)[1], field_name, bound, args, kwargs, result)
    if not found:
        return (True, False, None)
    return (True, True, _cache_scope_key(value))


def _hicache_seq_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("hicache_seq:"):
        return (False, False, None)
    found, value = _extract_source_value(source.split(":", 1)[1], field_name, bound, args, kwargs, result)
    if not found:
        return (True, False, None)
    scope = _cache_scope_key(value)
    next_seq = _HICACHE_SEQUENCE_BY_SCOPE.get(scope, 0) + 1
    _HICACHE_SEQUENCE_BY_SCOPE[scope] = next_seq
    return (True, True, next_seq)


def _hicache_config_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("hicache_config:"):
        return (False, False, None)
    parts = [part.strip() for part in source.split(":", 1)[1].split(",") if part.strip()]
    if not parts:
        return (True, False, None)
    found, value = _extract_source_value(parts[0], field_name, bound, args, kwargs, result)
    if not found:
        return (True, False, None)
    config = _cache_config_record(value)
    if len(parts) == 1:
        return (True, True, config)
    selected = config.get(parts[1])
    return (True, selected is not None, selected)


def _hicache_requested_pages_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("hicache_requested_pages:"):
        return (False, False, None)
    parts = [part.strip() for part in source.split(":", 1)[1].split(",") if part.strip()]
    if len(parts) < 2:
        return (True, False, None)
    found_tokens, token_value = _extract_source_value(parts[0], field_name, bound, args, kwargs, result)
    found_cache, cache_value = _extract_source_value(parts[1], "cache_scope", bound, args, kwargs, result)
    requested_tokens = _safe_int(token_value) if found_tokens else None
    page_size = _safe_int(getattr(cache_value, "page_size", None)) if found_cache else None
    if requested_tokens is None:
        return (True, False, None)
    return (
        True,
        True,
        {
            "requested_tokens": requested_tokens,
            "source_page_size": page_size,
            "requested_pages": _ceil_div(requested_tokens, page_size),
        },
    )


def _extract_token_path(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if not parts:
        return (False, None)
    found, value = _extract_source_value(parts[0], "token_path", bound, args, kwargs, result)
    if not found:
        return (False, None)
    scope = _scope_from_optional_source(parts[1], bound, args, kwargs, result) if len(parts) > 1 else ""
    return (True, _token_path_record(_tokens_for_path(value), scope))


def _extract_token_span(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if not parts:
        return (False, None)
    found, value = _extract_source_value(parts[0], "token_span", bound, args, kwargs, result)
    if not found:
        return (False, None)
    tokens = _tokens_for_path(value)
    return (True, _token_span_record(tokens, 0, len(tokens)))


def _extract_token_path_concat(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if len(parts) < 2:
        return (False, None)
    found_prefix, prefix = _extract_source_value(parts[0], "prefix_tokens", bound, args, kwargs, result)
    found_suffix, suffix = _extract_source_value(parts[1], "suffix_tokens", bound, args, kwargs, result)
    if not found_prefix or not found_suffix:
        return (False, None)
    scope = _scope_from_optional_source(parts[2], bound, args, kwargs, result) if len(parts) > 2 else ""
    tokens = _tokens_for_path(prefix) + _tokens_for_path(suffix)
    return (True, _token_path_record(tokens, scope))


def _extract_token_span_concat(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if len(parts) < 2:
        return (False, None)
    found_prefix, prefix = _extract_source_value(parts[0], "prefix_tokens", bound, args, kwargs, result)
    found_suffix, suffix = _extract_source_value(parts[1], "suffix_tokens", bound, args, kwargs, result)
    if not found_prefix or not found_suffix:
        return (False, None)
    tokens = _tokens_for_path(prefix) + _tokens_for_path(suffix)
    return (True, _token_span_record(tokens, 0, len(tokens)))


def _extract_node_token_path(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if not parts:
        return (False, None)
    found, node = _extract_source_value(parts[0], "node_token_path", bound, args, kwargs, result)
    if not found or node is None:
        return (False, None)
    scope = _scope_from_optional_source(parts[1], bound, args, kwargs, result) if len(parts) > 1 else ""
    return (True, _token_path_record(_full_key_tokens(node), scope))


def _extract_node_token_span(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if not parts:
        return (False, None)
    found, node = _extract_source_value(parts[0], "node_token_span", bound, args, kwargs, result)
    if not found or node is None:
        return (False, None)
    tokens = _full_key_tokens(node)
    return (True, _token_span_record(tokens, 0, len(tokens)))


def _extract_node_token_path_concat(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if len(parts) < 2:
        return (False, None)
    found_node, node = _extract_source_value(parts[0], "prefix_node", bound, args, kwargs, result)
    found_suffix, suffix = _extract_source_value(parts[1], "suffix_tokens", bound, args, kwargs, result)
    if not found_node or node is None or not found_suffix:
        return (False, None)
    scope = _scope_from_optional_source(parts[2], bound, args, kwargs, result) if len(parts) > 2 else ""
    tokens = _full_key_tokens(node) + _tokens_for_path(suffix)
    return (True, _token_path_record(tokens, scope))


def _extract_node_token_span_concat(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if len(parts) < 2:
        return (False, None)
    found_node, node = _extract_source_value(parts[0], "prefix_node", bound, args, kwargs, result)
    found_suffix, suffix = _extract_source_value(parts[1], "suffix_tokens", bound, args, kwargs, result)
    if not found_node or node is None or not found_suffix:
        return (False, None)
    tokens = _full_key_tokens(node) + _tokens_for_path(suffix)
    return (True, _token_span_record(tokens, 0, len(tokens)))


def _extract_source_value(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    return _base._extract_raw_value(source, field_name, bound, args, kwargs, result)


def _scope_from_optional_source(
    source: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> str:
    found, value = _extract_source_value(source, "cache_scope", bound, args, kwargs, result)
    return _cache_scope_key(value) if found else ""


def _cache_scope_key(value: Any) -> str:
    rank = os.environ.get("RANK", os.environ.get("LOCAL_RANK", "unknown"))
    if value is None:
        return f"rank:{rank}:unknown"
    if isinstance(value, (str, int, float, bool)):
        return f"rank:{rank}:{value}"
    return f"rank:{rank}:{type(value).__name__}:{id(value)}"


def _token_span_record(tokens: list[Any], begin: int, end: int) -> dict[str, Any]:
    return {
        "path_id": _token_path_id(tokens),
        "begin": begin,
        "end": end,
        "token_count": len(tokens),
        "hash_algo": _TOKEN_HASH_ALGO,
    }


def _token_path_record(tokens: list[Any], scope: str = "") -> dict[str, Any]:
    path_id = _token_path_id(tokens)
    row: dict[str, Any] = {
        "token_path_id": path_id,
        "token_count": len(tokens),
        "hash_algo": _TOKEN_HASH_ALGO,
    }
    scope_key = scope or "global"
    emitted = _TOKEN_PATHS_EMITTED_BY_SCOPE.setdefault(scope_key, set())
    if path_id not in emitted:
        emitted.add(path_id)
        row["token_ids"] = _jsonable_token_ids(tokens)
    return row


def _token_path_id(tokens: list[Any]) -> str:
    hasher = hashlib.sha256()
    for token in tokens:
        _hash_one_token_id(hasher, token)
    return "sha256_u32le:" + hasher.hexdigest()


def _jsonable_token_ids(tokens: list[Any]) -> list[Any]:
    result: list[Any] = []
    for token in tokens:
        if isinstance(token, (list, tuple)):
            result.append([int(item) for item in token])
        else:
            result.append(int(token))
    return result


def _hash_one_token_id(hasher: "hashlib._Hash", token: Any) -> None:
    if isinstance(token, (list, tuple)):
        for item in token:
            hasher.update(int(item).to_bytes(4, byteorder="little", signed=False))
    else:
        hasher.update(int(token).to_bytes(4, byteorder="little", signed=False))


def _safe_int(value: Any) -> int | None:
    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _tokens_for_path(value: Any) -> list[Any]:
    """读取完整 token 序列。

    通用 probe 的 `_safe_list` 会截断到 64 个元素，适合作摘要；token dictionary
    需要完整 token 序列，不能在这里截断。
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


def _cache_config_record(obj: Any) -> dict[str, Any]:
    capacity = _snapshot_capacity(obj)
    thresholds = {
        "write_through_threshold": capacity.get("write_through_threshold"),
        "load_back_threshold": capacity.get("load_back_threshold"),
        "prefetch_threshold_tokens": capacity.get("prefetch_threshold_tokens"),
        "prefetch_threshold_pages": capacity.get("prefetch_threshold_pages"),
        "prefetch_capacity_limit_tokens": capacity.get("prefetch_capacity_limit_tokens"),
        "prefetch_capacity_limit_pages": capacity.get("prefetch_capacity_limit_pages"),
        "storage_batch_size": capacity.get("storage_batch_size"),
    }
    capacity_summary = {
        "l1_capacity_tokens": capacity.get("l1_capacity_tokens"),
        "l1_capacity_pages": capacity.get("l1_capacity_pages"),
        "l1_available_tokens": capacity.get("l1_available_tokens"),
        "l1_available_pages": capacity.get("l1_available_pages"),
        "l2_capacity_tokens": capacity.get("l2_capacity_tokens"),
        "l2_capacity_pages": capacity.get("l2_capacity_pages"),
        "l2_available_tokens": capacity.get("l2_available_tokens"),
        "l2_available_pages": capacity.get("l2_available_pages"),
        "enable_storage": capacity.get("enable_storage"),
    }
    policy_params = {
        "write_policy": capacity.get("write_policy"),
        "prefetch_policy": capacity.get("prefetch_policy"),
        "thresholds": thresholds,
    }
    return {
        "source_page_size": capacity.get("page_size"),
        "write_policy": capacity.get("write_policy"),
        "prefetch_policy": capacity.get("prefetch_policy"),
        "thresholds": thresholds,
        "capacity_summary": capacity_summary,
        "policy_params": policy_params,
    }


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


def _full_key_tokens(node: Any, cache: dict[int, list[Any]] | None = None) -> list[Any]:
    if node is None:
        return []
    identity = id(node)
    if cache is not None and identity in cache:
        return cache[identity]
    chain = []
    seen: set[int] = set()
    current = node
    while current is not None and id(current) not in seen:
        seen.add(id(current))
        chain.append(current)
        current = getattr(current, "parent", None)
    tokens: list[Any] = []
    for item in reversed(chain):
        tokens.extend(_tokens_for_path(getattr(item, "key", getattr(item, "token_ids", None))))
    if cache is not None:
        cache[identity] = tokens
    return tokens


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


def _ceil_div(value: int | None, divisor: int | None) -> int | None:
    if value is None or divisor is None or divisor <= 0:
        return None
    return (value + divisor - 1) // divisor


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


_base.register_source_extractor(_hicache_state_source)
_base.register_source_extractor(_token_path_source)
_base.register_source_extractor(_token_span_source)
_base.register_source_extractor(_token_path_concat_source)
_base.register_source_extractor(_token_span_concat_source)
_base.register_source_extractor(_node_token_path_source)
_base.register_source_extractor(_node_token_span_source)
_base.register_source_extractor(_node_token_count_source)
_base.register_source_extractor(_node_token_path_concat_source)
_base.register_source_extractor(_node_token_span_concat_source)
_base.register_source_extractor(_hicache_cache_scope_source)
_base.register_source_extractor(_hicache_seq_source)
_base.register_source_extractor(_hicache_config_source)
_base.register_source_extractor(_hicache_requested_pages_source)

install = _base.install
TARGET_MODULES = _base.TARGET_MODULES
