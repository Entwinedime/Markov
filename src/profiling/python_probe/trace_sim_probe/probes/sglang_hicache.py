from __future__ import annotations

import hashlib
import os
from typing import Any, Dict, Optional, Tuple

from trace_sim_probe.patching import compact_id, safe_getattr, safe_len, wrap_method
from trace_sim_probe.schema import (
    FRAMEWORK_SGLANG,
    HICACHE_BACKUP_L1_TO_L2,
    HICACHE_CATEGORY,
    HICACHE_EVICT_L1,
    HICACHE_EVICT_L2,
    HICACHE_LOAD_L2_TO_L1,
    HICACHE_MATCH,
    HICACHE_PREFETCH_L3_TO_L2,
    HICACHE_PREFETCH_QUERY,
    HICACHE_WRITE_L2_TO_L3,
)
from trace_sim_probe.writer import get_writer


TARGET_MODULES = (
    "sglang.srt.mem_cache.hiradix_cache",
    "sglang.srt.mem_cache.unified_radix_cache",
    "sglang.srt.mem_cache.hi_mamba_radix_cache",
    "sglang.srt.managers.cache_controller",
    "sglang.srt.mem_cache.hybrid_cache.hybrid_cache_controller",
)


METHOD_EVENTS = {
    "match_prefix": (HICACHE_MATCH, "", "", "lookup", "query"),
    "prefetch_from_storage": (HICACHE_PREFETCH_QUERY, "L3", "L2", "prefetch", "query"),
    "prefetch": (HICACHE_PREFETCH_QUERY, "L3", "L2", "prefetch", "queue"),
    "_storage_hit_query": (HICACHE_PREFETCH_QUERY, "L3", "L2", "query", "query"),
    "_page_transfer": (HICACHE_PREFETCH_L3_TO_L2, "L3", "L2", "prefetch", "movement"),
    "_page_get_zero_copy": (HICACHE_PREFETCH_L3_TO_L2, "L3", "L2", "prefetch", "movement"),
    "_generic_page_get": (HICACHE_PREFETCH_L3_TO_L2, "L3", "L2", "prefetch", "movement"),
    "_draft_page_get": ("HiCache::draft_l3_to_l2", "L3", "L2", "prefetch", "movement"),
    "check_prefetch_progress": ("HiCache::prefetch_progress", "", "", "control", "control"),
    "load": (HICACHE_LOAD_L2_TO_L1, "L2", "L1", "load", "queue"),
    "load_back": (HICACHE_LOAD_L2_TO_L1, "L2", "L1", "load", "movement"),
    "start_loading": (HICACHE_LOAD_L2_TO_L1, "L2", "L1", "load", "queue"),
    "ready_to_load_host_cache": (HICACHE_LOAD_L2_TO_L1, "L2", "L1", "load", "control"),
    "write": (HICACHE_BACKUP_L1_TO_L2, "L1", "L2", "backup", "queue"),
    "write_backup": (HICACHE_BACKUP_L1_TO_L2, "L1", "L2", "backup", "movement"),
    "start_writing": (HICACHE_BACKUP_L1_TO_L2, "L1", "L2", "backup", "queue"),
    "write_storage": (HICACHE_WRITE_L2_TO_L3, "L2", "L3", "write", "queue"),
    "write_backup_storage": (HICACHE_WRITE_L2_TO_L3, "L2", "L3", "write", "queue"),
    "_page_backup": (HICACHE_WRITE_L2_TO_L3, "L2", "L3", "write", "movement"),
    "_page_set_zero_copy": (HICACHE_WRITE_L2_TO_L3, "L2", "L3", "write", "movement"),
    "_generic_page_set": (HICACHE_WRITE_L2_TO_L3, "L2", "L3", "write", "movement"),
    "_draft_page_set": ("HiCache::draft_l2_to_l3", "L2", "L3", "write", "movement"),
    "evict_device": (HICACHE_EVICT_L1, "L1", "", "evict", "movement"),
    "evict_host": (HICACHE_EVICT_L2, "L2", "", "evict", "movement"),
    "evict": (HICACHE_EVICT_L1, "L1", "", "evict", "queue"),
    "_evict_backuped": (HICACHE_EVICT_L1, "L1", "", "evict", "movement"),
    "_evict_regular": (HICACHE_EVICT_L1, "L1", "", "evict", "movement"),
    "insert": ("HiCache::insert_l1", "", "L1", "insert", "movement"),
    "_insert_helper_host": (HICACHE_PREFETCH_L3_TO_L2, "L3", "L2", "prefetch", "movement"),
    "init_load_back": (HICACHE_LOAD_L2_TO_L1, "L2", "L1", "load", "queue"),
    "terminate_prefetch": ("HiCache::terminate_prefetch", "", "", "control", "control"),
    "pop_prefetch_loaded_tokens": ("HiCache::prefetch_loaded_tokens", "", "", "ack", "ack"),
    "append_host_mem_release": (HICACHE_EVICT_L2, "L2", "", "release", "movement"),
    "_append_host_mem_release_pages": (HICACHE_EVICT_L2, "L2", "", "release", "movement"),
    "attach_storage_backend": ("HiCache::attach_storage_backend", "", "L3", "control", "control"),
    "detach_storage_backend": ("HiCache::detach_storage_backend", "L3", "", "control", "control"),
    "clear_storage_backend": ("HiCache::clear_storage_backend", "L3", "", "control", "control"),
    "check_hicache_events": ("HiCache::check_events", "", "", "control", "control"),
    "drain_storage_control_queues": ("HiCache::drain_storage_control_queues", "", "", "control", "control"),
    "release_aborted_request": ("HiCache::release_aborted_request", "", "", "control", "control"),
    "flush_write_through_acks": ("HiCache::flush_write_acks", "", "", "control", "ack"),
    "prefetch_rate_limited": ("HiCache::prefetch_rate_limited", "", "", "control", "control"),
}


CLASS_METHODS = {
    "HiRadixCache": (
        "match_prefix",
        "prefetch_from_storage",
        "check_prefetch_progress",
        "write_backup",
        "write_backup_storage",
        "load_back",
        "ready_to_load_host_cache",
        "flush_write_through_acks",
        "check_hicache_events",
        "drain_storage_control_queues",
        "evict",
        "evict_host",
        "_evict_backuped",
        "_evict_regular",
        "insert",
        "_insert_helper_host",
        "init_load_back",
        "terminate_prefetch",
        "pop_prefetch_loaded_tokens",
        "release_aborted_request",
    ),
    "UnifiedRadixCache": (
        "match_prefix",
        "prefetch_from_storage",
        "check_prefetch_progress",
        "write_backup",
        "write_backup_storage",
        "load_back",
        "ready_to_load_host_cache",
        "flush_write_through_acks",
        "check_hicache_events",
        "drain_storage_control_queues",
        "evict",
        "evict_host",
        "insert",
        "terminate_prefetch",
        "pop_prefetch_loaded_tokens",
        "release_aborted_request",
    ),
    "HiMambaRadixCache": (
        "match_prefix",
        "prefetch_from_storage",
        "check_prefetch_progress",
        "write_backup",
        "write_backup_storage",
        "load_back",
        "ready_to_load_host_cache",
        "flush_write_through_acks",
        "check_hicache_events",
        "drain_storage_control_queues",
        "evict",
        "evict_host",
        "insert",
        "terminate_prefetch",
        "pop_prefetch_loaded_tokens",
        "release_aborted_request",
    ),
    "HiCacheController": (
        "write",
        "start_writing",
        "load",
        "start_loading",
        "evict_device",
        "evict_host",
        "prefetch",
        "terminate_prefetch",
        "append_host_mem_release",
        "_append_host_mem_release_pages",
        "_storage_hit_query",
        "_page_transfer",
        "_page_get_zero_copy",
        "_generic_page_get",
        "prefetch_rate_limited",
        "write_storage",
        "_page_backup",
        "_page_set_zero_copy",
        "_generic_page_set",
        "_draft_page_get",
        "_draft_page_set",
        "attach_storage_backend",
        "detach_storage_backend",
    ),
    "HybridCacheController": (
        "write",
        "start_writing",
        "load",
        "start_loading",
        "evict_device",
        "evict_host",
        "prefetch",
        "terminate_prefetch",
        "append_host_mem_release",
        "_append_host_mem_release_pages",
        "_storage_hit_query",
        "_page_transfer",
        "_page_get_zero_copy",
        "_generic_page_get",
        "write_storage",
        "_page_backup",
        "_page_set_zero_copy",
        "_generic_page_set",
        "attach_storage_backend",
        "clear_storage_backend",
    ),
}


def _tensor_count(value: Any) -> Optional[int]:
    count = safe_len(value)
    if count is not None:
        return count
    try:
        return int(value.numel())
    except Exception:
        return None


def _first_count(*values: Any) -> Optional[int]:
    for value in values:
        count = _tensor_count(value)
        if count is not None:
            return count
    return None


def _probe_key_mode() -> str:
    mode = os.environ.get("TRACE_SIM_PYTHON_PROBE_KEY_MODE", "none").strip().lower()
    return mode if mode in ("none", "hash", "raw") else "none"


def _max_keys() -> int:
    try:
        return max(0, int(os.environ.get("TRACE_SIM_PYTHON_PROBE_MAX_KEYS_PER_EVENT", "4096")))
    except Exception:
        return 4096


def _first_attr(obj: Any, *names: str) -> Any:
    for name in names:
        value = safe_getattr(obj, name, None)
        if value is not None:
            return value
    return None


def _as_int(value: Any) -> Optional[int]:
    if value is None:
        return None
    try:
        return int(value)
    except Exception:
        return None


def _dtype_bytes(dtype: Any) -> Optional[int]:
    if dtype is None:
        return None
    text = str(dtype).lower()
    if "float64" in text or "double" in text or "int64" in text:
        return 8
    if "float32" in text or "int32" in text:
        return 4
    if "float16" in text or "bfloat16" in text or "half" in text or "int16" in text:
        return 2
    if "int8" in text or "uint8" in text or "fp8" in text:
        return 1
    return None


def _infer_bytes_per_page(self_obj: Any, page_size: Any) -> Optional[int]:
    direct = _as_int(_first_attr(self_obj, "bytes_per_page", "page_bytes", "page_nbytes", "kv_page_bytes"))
    if direct:
        return direct

    for nested_name in ("mem_pool_host", "mem_pool_device", "host_mem_pool", "device_mem_pool", "storage_backend"):
        nested = safe_getattr(self_obj, nested_name, None)
        direct = _as_int(_first_attr(nested, "bytes_per_page", "page_bytes", "page_nbytes", "kv_page_bytes"))
        if direct:
            return direct

    page = _as_int(page_size)
    if not page:
        return None

    model_config = safe_getattr(self_obj, "model_config", None)
    num_layers = _as_int(
        _first_attr(self_obj, "num_layers", "layer_num", "num_hidden_layers")
        or _first_attr(model_config, "num_hidden_layers", "num_layers")
    )
    num_kv_heads = _as_int(
        _first_attr(self_obj, "num_kv_heads", "num_key_value_heads", "num_heads")
        or _first_attr(model_config, "num_key_value_heads", "num_attention_heads")
    )
    head_dim = _as_int(_first_attr(self_obj, "head_dim", "kv_head_dim") or _first_attr(model_config, "head_dim"))
    dtype_bytes = _dtype_bytes(
        _first_attr(self_obj, "dtype", "kv_cache_dtype", "cache_dtype")
        or _first_attr(model_config, "torch_dtype", "dtype", "kv_cache_dtype")
    )

    if num_layers and num_kv_heads and head_dim and dtype_bytes:
        return page * num_layers * num_kv_heads * head_dim * 2 * dtype_bytes
    return None


def _safe_iter(value: Any):
    if value is None:
        return []
    if isinstance(value, (str, bytes)):
        return [value]
    try:
        return list(value)
    except Exception:
        return [value]


def _hash_key(value: Any) -> str:
    return hashlib.sha1(str(value).encode("utf-8", errors="replace")).hexdigest()[:24]


def _encode_keys(keys: Any, page_size: Any = None) -> Tuple[Optional[str], bool]:
    mode = _probe_key_mode()
    if mode == "none" or keys is None:
        return None, False

    values = _safe_iter(keys)
    page = _as_int(page_size)
    if page and values and not isinstance(values[0], str):
        grouped = [tuple(values[i : i + page]) for i in range(0, len(values), page)]
        values = grouped

    max_keys = _max_keys()
    truncated = max_keys > 0 and len(values) > max_keys
    if max_keys > 0:
        values = values[:max_keys]

    if mode == "raw":
        encoded = [str(value) for value in values]
    else:
        encoded = [_hash_key(value) for value in values]
    return "|".join(encoded), truncated


def _node_token_count(node: Any) -> Optional[int]:
    for attr in ("value", "host_value"):
        count = _tensor_count(safe_getattr(node, attr, None))
        if count is not None:
            return count
    key = safe_getattr(node, "key", None)
    return _tensor_count(key)


def _node_keys(node: Any) -> Any:
    hash_value = safe_getattr(node, "hash_value", None)
    if hash_value is not None:
        return hash_value
    key = safe_getattr(node, "key", None)
    token_ids = safe_getattr(key, "token_ids", None)
    return token_ids if token_ids is not None else key


def _node_id(node: Any) -> Any:
    return safe_getattr(node, "id", None)


def _pool_transfer_names(pool_transfers: Any) -> Tuple[Optional[str], int]:
    names = []
    for transfer in _safe_iter(pool_transfers):
        name = safe_getattr(transfer, "name", None)
        if name is not None:
            names.append(str(name))
    return ("|".join(names) if names else None, len(names))


def _extra_pool_event_args(base_args: Dict[str, Any], pool_transfers: Any):
    events = []
    for transfer in _safe_iter(pool_transfers):
        name = safe_getattr(transfer, "name", None)
        host_indices = safe_getattr(transfer, "host_indices", None)
        device_indices = safe_getattr(transfer, "device_indices", None)
        keys = safe_getattr(transfer, "keys", None)
        count = _first_count(host_indices, device_indices, keys)
        if count is None:
            continue
        event_args = dict(base_args)
        event_args["event_kind"] = "movement"
        event_args["pool"] = str(name) if name is not None else "extra"
        event_args["pool_name"] = str(name) if name is not None else "extra"
        event_args["transfer_scope"] = "extra_pool"
        event_args["num_tokens"] = count
        page_size = event_args.get("page_size")
        try:
            event_args["num_pages"] = (int(count) + int(page_size) - 1) // int(page_size) if page_size else count
        except Exception:
            event_args["num_pages"] = count
        page_keys_hash, key_truncated = _encode_keys(keys, page_size)
        event_args["page_keys_hash"] = page_keys_hash
        event_args["key_truncated"] = key_truncated
        events.append(event_args)
    return events


def _arg(args: Tuple[Any, ...], index: int, kwargs: Dict[str, Any], name: str) -> Any:
    if name in kwargs:
        return kwargs[name]
    if index < len(args):
        return args[index]
    return None


def _sum_operation_indices(operations: Any, *attr_names: str) -> Optional[int]:
    try:
        iterable = list(operations)
    except Exception:
        return None
    total = 0
    found = False
    for operation in iterable:
        for attr_name in attr_names:
            count = _tensor_count(safe_getattr(operation, attr_name, None))
            if count is not None:
                total += count
                found = True
                break
    return total if found else None


def _prefetch_operation_for(self_obj: Any, request_id: Any) -> Any:
    if request_id is None:
        return None
    ongoing = safe_getattr(self_obj, "ongoing_prefetch", None)
    try:
        info = ongoing.get(request_id) if ongoing is not None else None
    except Exception:
        return None
    if isinstance(info, tuple) and len(info) >= 4:
        return info[3]
    return None


def _operation_keys(operation: Any) -> Any:
    hash_value = safe_getattr(operation, "hash_value", None)
    if hash_value:
        return hash_value
    return safe_getattr(operation, "token_ids", None)


def _base_args(self_obj: Any, method_name: str, args: Tuple[Any, ...], kwargs: Dict[str, Any]) -> Dict[str, Any]:
    page_size = safe_getattr(self_obj, "page_size", None)
    storage_backend = safe_getattr(self_obj, "storage_backend_type", None)
    storage_obj = safe_getattr(self_obj, "storage_backend", None)
    if storage_backend is None and storage_obj is not None:
        storage_backend = type(storage_obj).__name__

    request_id = (
        kwargs.get("req_id")
        or kwargs.get("request_id")
        or (_arg(args, 0, kwargs, "req_id") if method_name in ("check_prefetch_progress", "terminate_prefetch", "pop_prefetch_loaded_tokens") else None)
        or (_arg(args, 0, kwargs, "request_id") if method_name == "prefetch" else None)
    )

    host_indices = kwargs.get("host_indices")
    device_indices = kwargs.get("device_indices")
    token_ids = kwargs.get("token_ids") or kwargs.get("new_input_tokens")
    page_keys = None
    node_id = None
    operation_id = None
    pool_transfers = kwargs.get("extra_pools")
    transfer_scope = "kv"
    inferred_num_tokens = None
    if method_name in ("load", "append_host_mem_release"):
        host_indices = _arg(args, 0, kwargs, "host_indices")
    elif method_name in ("write", "evict_device"):
        device_indices = _arg(args, 0, kwargs, "device_indices")
    elif method_name == "prefetch":
        host_indices = _arg(args, 1, kwargs, "host_indices")
        token_ids = _arg(args, 2, kwargs, "new_input_tokens")
    elif method_name == "prefetch_from_storage":
        token_ids = _arg(args, 2, kwargs, "new_input_tokens")
    elif method_name == "write_storage":
        host_indices = _arg(args, 0, kwargs, "host_indices")
        token_ids = _arg(args, 1, kwargs, "token_ids")
        page_keys = _arg(args, 2, kwargs, "hash_value") or token_ids
    elif method_name == "start_loading":
        num_tokens_from_queue = _sum_operation_indices(safe_getattr(self_obj, "load_queue", None), "host_indices", "device_indices")
        if num_tokens_from_queue is not None:
            inferred_num_tokens = num_tokens_from_queue
    elif method_name == "start_writing":
        num_tokens_from_queue = _sum_operation_indices(safe_getattr(self_obj, "write_queue", None), "device_indices", "host_indices")
        if num_tokens_from_queue is not None:
            inferred_num_tokens = num_tokens_from_queue
    elif method_name in ("_page_transfer", "_page_backup", "terminate_prefetch"):
        operation = _arg(args, 0, kwargs, "operation")
        operation_id = safe_getattr(operation, "id", None)
        host_indices = safe_getattr(operation, "host_indices", None)
        token_ids = safe_getattr(operation, "token_ids", None) or safe_getattr(operation, "hash_value", None)
        request_id = request_id or safe_getattr(operation, "request_id", None)
        page_keys = _operation_keys(operation)
        pool_transfers = safe_getattr(operation, "pool_transfers", None)
    elif method_name in ("_page_get_zero_copy", "_generic_page_get"):
        operation = _arg(args, 0, kwargs, "operation")
        hash_values = _arg(args, 1, kwargs, "hash_values")
        host_indices = _arg(args, 2, kwargs, "host_indices")
        operation_id = safe_getattr(operation, "id", None)
        request_id = safe_getattr(operation, "request_id", None)
        page_keys = hash_values
        num_hashes = _tensor_count(hash_values)
        inferred_num_tokens = num_hashes * int(page_size) if num_hashes is not None and page_size else None
    elif method_name in ("_page_set_zero_copy", "_generic_page_set", "_draft_page_set", "_draft_page_get"):
        hash_values = _arg(args, 0, kwargs, "hash_values")
        host_indices = _arg(args, 1, kwargs, "host_indices")
        page_keys = hash_values
        num_hashes = _tensor_count(hash_values)
        inferred_num_tokens = num_hashes * int(page_size) if num_hashes is not None and page_size else None
    elif method_name == "check_prefetch_progress":
        operation = _prefetch_operation_for(self_obj, request_id)
        if operation is not None:
            host_indices = safe_getattr(operation, "host_indices", None)
            token_ids = safe_getattr(operation, "hash_value", None)
            operation_id = safe_getattr(operation, "id", None)
            page_keys = _operation_keys(operation)
    elif method_name in ("write_backup", "write_backup_storage", "load_back", "_evict_backuped", "_evict_regular"):
        node = _arg(args, 0, kwargs, "node")
        node_id = _node_id(node)
        page_keys = _node_keys(node)
        inferred_num_tokens = _node_token_count(node)
        host_indices = safe_getattr(node, "host_value", None)
        device_indices = safe_getattr(node, "value", None)
    elif method_name == "_insert_helper_host":
        node = _arg(args, 0, kwargs, "node")
        key = _arg(args, 1, kwargs, "key")
        host_indices = _arg(args, 2, kwargs, "host_value")
        hash_value = _arg(args, 3, kwargs, "hash_value")
        node_id = _node_id(node)
        page_keys = hash_value or safe_getattr(key, "token_ids", None) or key
        inferred_num_tokens = _tensor_count(host_indices) or _tensor_count(key)
    elif method_name == "insert":
        params = _arg(args, 0, kwargs, "params")
        key = safe_getattr(params, "key", None)
        value = safe_getattr(params, "value", None)
        page_keys = safe_getattr(key, "token_ids", None) or key
        inferred_num_tokens = _tensor_count(value) or _tensor_count(key)
    elif method_name == "evict_host":
        inferred_num_tokens = _as_int(_arg(args, 0, kwargs, "num_tokens"))
    elif method_name == "_append_host_mem_release_pages":
        host_indices = _arg(args, 1, kwargs, "host_indices")
        page_size = _arg(args, 2, kwargs, "page_size") or page_size

    num_tokens = inferred_num_tokens if inferred_num_tokens is not None else _first_count(host_indices, device_indices, token_ids)
    num_pages = None
    try:
        if num_tokens is not None and page_size:
            num_pages = (int(num_tokens) + int(page_size) - 1) // int(page_size)
    except Exception:
        num_pages = None
    bytes_per_page = _infer_bytes_per_page(self_obj, page_size)
    bytes_moved = num_pages * bytes_per_page if num_pages and bytes_per_page else None

    event_name, tier_src, tier_dst, direction, event_kind = METHOD_EVENTS.get(method_name, (f"HiCache::{method_name}", "", "", "runtime", "control"))
    page_keys_hash, key_truncated = _encode_keys(page_keys, page_size)
    pool_name, pool_transfer_count = _pool_transfer_names(pool_transfers)
    if pool_transfer_count:
        transfer_scope = "kv+extra_pool"
    return {
        "framework": FRAMEWORK_SGLANG,
        "producer": "python_probe",
        "domain": "cache_io",
        "event_kind": event_kind,
        "python_module": type(self_obj).__module__,
        "python_class": type(self_obj).__name__,
        "python_method": method_name,
        "op_id": compact_id(self_obj),
        "operation_id": operation_id,
        "node_id": node_id,
        "request_id": request_id,
        "tier_src": tier_src,
        "tier_dst": tier_dst,
        "direction": direction,
        "num_tokens": num_tokens,
        "num_pages": num_pages,
        "page_size": page_size,
        "bytes_per_page": bytes_per_page,
        "bytes": bytes_moved,
        "page_keys_hash": page_keys_hash,
        "key_truncated": key_truncated,
        "io_backend": safe_getattr(self_obj, "io_backend", None),
        "storage_backend": storage_backend,
        "write_policy": safe_getattr(self_obj, "write_policy", None),
        "pool": type(safe_getattr(self_obj, "mem_pool_host", None)).__name__ if safe_getattr(self_obj, "mem_pool_host", None) is not None else None,
        "pool_name": pool_name or "KV",
        "pool_transfer_count": pool_transfer_count,
        "transfer_scope": transfer_scope,
        "_pool_transfers": pool_transfers,
        "_event_name": event_name,
    }


def _wrapper(method_name: str):
    def make(original):
        def wrapped(self_obj, *args, **kwargs):
            event_args = _base_args(self_obj, method_name, args, kwargs)
            event_name = event_args.pop("_event_name")
            pool_transfers = event_args.pop("_pool_transfers", None)
            writer = get_writer()
            start = writer.now_us()
            try:
                result = original(self_obj, *args, **kwargs)
                event_args["status"] = "ok"
                if method_name == "match_prefix":
                    host_hit = safe_getattr(result, "host_hit_length", None)
                    device = safe_getattr(result, "device_indices", None)
                    if host_hit is not None:
                        event_args["host_hit_tokens"] = host_hit
                    if device is not None:
                        event_args["device_hit_tokens"] = _tensor_count(device)
                elif method_name == "prefetch_rate_limited":
                    event_args["rate_limited"] = bool(result)
                elif method_name == "pop_prefetch_loaded_tokens":
                    event_args["loaded_from_storage_tokens"] = result
                    try:
                        loaded_tokens = int(result)
                        event_args["num_tokens"] = loaded_tokens
                        page_size = event_args.get("page_size")
                        if page_size:
                            event_args["num_pages"] = (loaded_tokens + int(page_size) - 1) // int(page_size)
                    except Exception:
                        pass
                elif method_name == "_storage_hit_query":
                    try:
                        hash_values, hit_tokens = result
                        event_args["storage_hit_tokens"] = int(hit_tokens)
                        event_args["num_tokens"] = int(hit_tokens)
                        page_size = event_args.get("page_size")
                        if page_size:
                            event_args["num_pages"] = (int(hit_tokens) + int(page_size) - 1) // int(page_size)
                        page_keys_hash, key_truncated = _encode_keys(hash_values, page_size)
                        event_args["page_keys_hash"] = page_keys_hash
                        event_args["key_truncated"] = key_truncated
                    except Exception:
                        pass
                return result
            except Exception as exc:
                event_args["status"] = "exception"
                event_args["exception"] = type(exc).__name__
                raise
            finally:
                writer.duration_event(event_name, start, writer.now_us(), HICACHE_CATEGORY, event_args)
                for pool_event_args in _extra_pool_event_args(event_args, pool_transfers):
                    writer.duration_event("HiCache::extra_pool_transfer", start, writer.now_us(), HICACHE_CATEGORY, pool_event_args)

        return wrapped

    return make


def install(module) -> bool:
    changed = False
    for class_name, methods in CLASS_METHODS.items():
        cls = safe_getattr(module, class_name)
        if cls is None:
            continue
        for method_name in methods:
            changed = wrap_method(cls, method_name, _wrapper(method_name)) or changed
    return changed
