from __future__ import annotations

import hashlib
import itertools
import os
import threading
from typing import Any, Dict, Optional, Tuple

from trace_sim_probe.patching import compact_id, safe_getattr, safe_len, wrap_method
from trace_sim_probe.schema import (
    FRAMEWORK_SGLANG,
    HICACHE_BACKUP_L1_TO_L2,
    HICACHE_CACHE_OPERATION,
    HICACHE_CATEGORY,
    HICACHE_EVICT_L1,
    HICACHE_EVICT_L2,
    HICACHE_LOAD_L2_TO_L1,
    HICACHE_MATCH,
    HICACHE_PREFETCH_L3_TO_L2,
    HICACHE_PREFETCH_QUERY,
    HICACHE_RADIX_OP,
    HICACHE_RADIX_SCHEMA,
    HICACHE_STORAGE_OP,
    HICACHE_WRITE_L2_TO_L3,
)
from trace_sim_probe.writer import get_writer


TARGET_MODULES = (
    "sglang.srt.mem_cache.hiradix_cache",
    "sglang.srt.mem_cache.unified_radix_cache",
    "sglang.srt.mem_cache.hi_mamba_radix_cache",
    "sglang.srt.managers.cache_controller",
    "sglang.srt.mem_cache.hybrid_cache.hybrid_cache_controller",
    "sglang.srt.mem_cache.hicache_storage",
    "sglang.srt.mem_cache.storage.simm.hicache_simm",
    "sglang.srt.mem_cache.storage.mooncake_store.mooncake_store",
    "sglang.srt.mem_cache.storage.hf3fs.storage_hf3fs",
    "sglang.srt.mem_cache.storage.eic.eic_storage",
    "sglang.srt.mem_cache.storage.nixl.hicache_nixl",
    "sglang.srt.mem_cache.storage.aibrix_kvcache.aibrix_kvcache_storage",
)

_OP_SEQUENCE = itertools.count(1)
_STORAGE_CONTEXT = threading.local()
_OPERATION_CONTEXT = threading.local()
_LOAD_OPERATION_BY_NODE: Dict[str, str] = {}
_CACHE_ID_BY_OBJECT: Dict[str, str] = {}


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
    "start_loading": (HICACHE_LOAD_L2_TO_L1, "L2", "L1", "load", "movement"),
    "ready_to_load_host_cache": (HICACHE_LOAD_L2_TO_L1, "L2", "L1", "load", "control"),
    "write": (HICACHE_BACKUP_L1_TO_L2, "L1", "L2", "backup", "queue"),
    "write_backup": (HICACHE_BACKUP_L1_TO_L2, "L1", "L2", "backup", "movement"),
    "start_writing": (HICACHE_BACKUP_L1_TO_L2, "L1", "L2", "backup", "movement"),
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
    "prefetch_abort": ("HiCache::prefetch_abort", "", "", "control", "control"),
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

STORAGE_METHODS = (
    "batch_exists",
    "batch_exists_v2",
    "batch_get",
    "batch_get_v1",
    "batch_get_v2",
    "batch_set",
    "batch_set_v1",
    "batch_set_v2",
)

RADIX_MODEL_METHODS = {
    "match_prefix",
    "insert",
    "prefetch_from_storage",
    "write_backup",
    "write_backup_storage",
    "evict",
    "evict_host",
    "_evict_backuped",
    "_evict_regular",
    "load_back",
}

CACHE_OPERATION_METHODS = {
    "prefetch",
    "load",
    "write",
    "write_storage",
    "terminate_prefetch",
    "init_load_back",
    "load_back",
    "_storage_hit_query",
    "_insert_helper_host",
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
        "prefetch_abort",
        "pop_prefetch_loaded_tokens",
        "release_aborted_request",
        "attach_storage_backend",
        "detach_storage_backend",
        "clear_storage_backend",
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
        "_insert_helper_host",
        "init_load_back",
        "terminate_prefetch",
        "pop_prefetch_loaded_tokens",
        "release_aborted_request",
        "attach_storage_backend",
        "detach_storage_backend",
        "clear_storage_backend",
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
        "_evict_regular",
        "insert",
        "_insert_helper_host",
        "init_load_back",
        "terminate_prefetch",
        "prefetch_abort",
        "pop_prefetch_loaded_tokens",
        "release_aborted_request",
        "attach_storage_backend",
        "detach_storage_backend",
        "clear_storage_backend",
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
    return mode if mode in ("none", "hash", "raw", "block_hash") else "none"


def _block_size_tokens() -> int:
    try:
        return max(1, int(os.environ.get("TRACE_SIM_PYTHON_PROBE_BLOCK_SIZE_TOKENS", "32")))
    except Exception:
        return 32


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


def _id_text(value: Any) -> Optional[str]:
    if value is None:
        return None
    text = str(value)
    return text if text != "" else None


def _remember_cache_links(cache_obj: Any) -> str:
    cache_id = compact_id(cache_obj)
    _CACHE_ID_BY_OBJECT[cache_id] = cache_id
    for attr in ("cache_controller", "storage_backend"):
        linked = safe_getattr(cache_obj, attr, None)
        if linked is not None:
            _CACHE_ID_BY_OBJECT[compact_id(linked)] = cache_id
            nested_storage = safe_getattr(linked, "storage_backend", None)
            if nested_storage is not None:
                _CACHE_ID_BY_OBJECT[compact_id(nested_storage)] = cache_id
    return cache_id


def _cache_id_for_object(obj: Any) -> Optional[str]:
    if obj is None:
        return None
    class_name = type(obj).__name__
    if class_name == "HiRadixCache":
        return _remember_cache_links(obj)
    object_id = compact_id(obj)
    mapped = _CACHE_ID_BY_OBJECT.get(object_id)
    if mapped:
        return mapped
    storage = safe_getattr(obj, "storage_backend", None)
    if storage is not None:
        mapped = _CACHE_ID_BY_OBJECT.get(compact_id(storage))
        if mapped:
            _CACHE_ID_BY_OBJECT[object_id] = mapped
            return mapped
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


def _ceil_div(value: Any, divisor: Any) -> Optional[int]:
    numerator = _as_int(value)
    denominator = _as_int(divisor)
    if numerator is None or denominator is None or denominator <= 0:
        return None
    return (numerator + denominator - 1) // denominator


def _infer_bytes_per_page(self_obj: Any, page_size: Any) -> Optional[int]:
    direct = _as_int(_first_attr(self_obj, "bytes_per_page", "page_bytes", "page_nbytes", "kv_page_bytes"))
    if direct:
        return direct

    for nested_name in ("mem_pool_host", "mem_pool_device", "host_mem_pool", "device_mem_pool", "storage_backend"):
        nested = safe_getattr(self_obj, nested_name, None)
        direct = _as_int(_first_attr(nested, "bytes_per_page", "page_bytes", "page_nbytes", "kv_page_bytes"))
        if direct:
            return direct
        nested_page = _as_int(safe_getattr(nested, "page_size", None)) or _as_int(page_size)
        memory_per_token = safe_getattr(nested, "memory_per_token", None)
        if nested_page and callable(memory_per_token):
            try:
                per_token = _as_int(memory_per_token())
                if per_token:
                    return nested_page * per_token
            except Exception:
                pass
        token_stride_size = _as_int(safe_getattr(nested, "token_stride_size", None))
        layer_num = _as_int(safe_getattr(nested, "layer_num", None))
        if nested_page and token_stride_size and layer_num:
            kv_multiplier = 1 if type(nested).__name__.startswith("MLA") else 2
            return nested_page * token_stride_size * layer_num * kv_multiplier

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


def _encode_block_keys(keys: Any) -> Tuple[Optional[str], bool, int]:
    mode = _probe_key_mode()
    block_size = _block_size_tokens()
    if mode == "none" or keys is None:
        return None, False, block_size

    values = _safe_iter(keys)
    if not values:
        return "", False, block_size

    grouped = [tuple(values[i : i + block_size]) for i in range(0, len(values), block_size)]
    max_keys = _max_keys()
    truncated = max_keys > 0 and len(grouped) > max_keys
    if max_keys > 0:
        grouped = grouped[:max_keys]

    if mode == "raw":
        encoded = [str(value) for value in grouped]
    else:
        encoded = [_hash_key(value) for value in grouped]
    return "|".join(encoded), truncated, block_size


def _encoded_block_list(keys: Any) -> Tuple[list[str], bool, int]:
    mode = _probe_key_mode()
    block_size = _block_size_tokens()
    if mode == "none" or keys is None:
        return [], False, block_size

    values = _safe_iter(keys)
    if not values:
        return [], False, block_size

    grouped = [tuple(values[i : i + block_size]) for i in range(0, len(values), block_size)]
    max_keys = _max_keys()
    truncated = max_keys > 0 and len(grouped) > max_keys
    if max_keys > 0:
        grouped = grouped[:max_keys]

    if mode == "raw":
        encoded = [str(value) for value in grouped]
    else:
        encoded = [_hash_key(value) for value in grouped]
    return encoded, truncated, block_size


def _encode_trace_page_block_keys(keys: Any, page_size: Any) -> Tuple[Optional[str], bool, int]:
    blocks, truncated, block_size = _encoded_block_list(keys)
    page = _as_int(page_size)
    if not blocks or page is None or page <= 0:
        return None, truncated, block_size
    page_blocks = page // block_size
    if page_blocks <= 0:
        return None, truncated, block_size
    aligned_count = len(blocks) // page_blocks * page_blocks
    block_pages = [
        ",".join(blocks[start : start + page_blocks])
        for start in range(0, aligned_count, page_blocks)
    ]
    return "|".join(block_pages), truncated, block_size


def _pipe_count(value: Any) -> int:
    if value is None or value == "":
        return 0
    return len([part for part in str(value).split("|") if part])


def _truncate_pipe(value: Any, count: Optional[int]) -> Any:
    if value in (None, "") or count is None:
        return value
    parts = [part for part in str(value).split("|") if part]
    return "|".join(parts[: max(0, count)])


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


def _node_raw_keys(node: Any) -> Any:
    key = safe_getattr(node, "key", None)
    token_ids = safe_getattr(key, "token_ids", None)
    return token_ids if token_ids is not None else key


def _node_id(node: Any) -> Any:
    return safe_getattr(node, "id", None)


def _node_parent_id(node: Any) -> Any:
    parent = safe_getattr(node, "parent", None)
    return safe_getattr(parent, "id", None)


def _node_parent(node: Any) -> Any:
    return safe_getattr(node, "parent", None)


def _flatten_key_values(values: Any):
    flattened = []
    for value in _safe_iter(values):
        if value is None:
            continue
        if isinstance(value, (str, bytes)):
            flattened.append(value)
            continue
        try:
            flattened.extend(list(value))
        except Exception:
            flattened.append(value)
    return flattened


def _node_full_raw_keys(node: Any) -> Any:
    if node is None:
        return None
    parts = []
    seen = set()
    current = node
    while current is not None and id(current) not in seen:
        seen.add(id(current))
        raw = _node_raw_keys(current)
        if raw is not None:
            parts.append(raw)
        current = _node_parent(current)
    if not parts:
        return None
    flattened = []
    for raw in reversed(parts):
        flattened.extend(_flatten_key_values(raw))
    return flattened


def _bool_attr(obj: Any, name: str) -> Optional[bool]:
    value = safe_getattr(obj, name, None)
    if value is None:
        return None
    try:
        return bool(value)
    except Exception:
        return None


def _pool_transfer_names(pool_transfers: Any) -> Tuple[Optional[str], int]:
    names = []
    for transfer in _safe_iter(pool_transfers):
        name = safe_getattr(transfer, "name", None)
        if name is not None:
            names.append(str(name))
    return ("|".join(names) if names else None, len(names))


def _extra_pool_success_pages(pool_storage_result: Any, name: Any) -> Optional[int]:
    extra_pool_hit_pages = safe_getattr(pool_storage_result, "extra_pool_hit_pages", None)
    if not extra_pool_hit_pages:
        return None
    for key in (name, str(name), safe_getattr(name, "value", None)):
        if key is None:
            continue
        try:
            if key in extra_pool_hit_pages:
                return _as_int(extra_pool_hit_pages[key])
        except Exception:
            continue
    return None


def _extra_pool_event_args(base_args: Dict[str, Any], pool_transfers: Any, pool_storage_result: Any = None):
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
        success_pages = _extra_pool_success_pages(pool_storage_result, name)
        if success_pages is not None:
            event_args["storage_success_pages"] = success_pages
            requested_pages = _as_int(event_args.get("num_pages"))
            event_args["status"] = _status_from_progress(success_pages, requested_pages)
            tokens = success_pages * int(page_size) if page_size else success_pages
            _update_count_fields(event_args, tokens, success_pages)
        page_keys_hash, key_truncated = _encode_keys(keys, page_size)
        event_args["page_keys_hash"] = page_keys_hash
        event_args["key_truncated"] = key_truncated
        events.append(event_args)
    return events


def _join_values(values: Any) -> Optional[str]:
    collected = []
    for value in _safe_iter(values):
        if value is None:
            continue
        if isinstance(value, (list, tuple, set)):
            collected.extend(str(part) for part in value if part is not None)
        else:
            collected.append(str(value))
    return "|".join(collected) if collected else None


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


def _summarize_operations(operations: Any, *count_attrs: str) -> Dict[str, Any]:
    try:
        iterable = list(operations)
    except Exception:
        return {}

    total = 0
    found_count = False
    operation_ids = []
    node_ids = []
    pool_transfers = []
    for operation in iterable:
        operation_id = safe_getattr(operation, "id", None)
        if operation_id is not None:
            operation_ids.append(_id_text(operation_id))
        node_ids.extend(_id_text(node_id) for node_id in _safe_iter(safe_getattr(operation, "node_ids", None)) if _id_text(node_id) is not None)
        pool_transfers.extend(_safe_iter(safe_getattr(operation, "pool_transfers", None)))
        for attr_name in count_attrs:
            count = _tensor_count(safe_getattr(operation, attr_name, None))
            if count is not None:
                total += count
                found_count = True
                break

    return {
        "num_tokens": total if found_count else None,
        "operation_id": _join_values(operation_ids),
        "node_id": _join_values(node_ids),
        "pool_transfers": pool_transfers or None,
    }


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


def _remember_load_operations(operation_summary: Dict[str, Any]) -> None:
    operation_id = _id_text(operation_summary.get("operation_id"))
    if operation_id is None:
        return
    node_ids = operation_summary.get("node_id")
    if isinstance(node_ids, str):
        iterable = [part for part in node_ids.split("|") if part]
    else:
        iterable = _safe_iter(node_ids)
    for node_id in iterable:
        node_key = _id_text(node_id)
        if node_key is not None:
            _LOAD_OPERATION_BY_NODE[node_key] = operation_id


def _load_operation_for_node(node_id: Any) -> Optional[str]:
    node_key = _id_text(node_id)
    if node_key is None:
        return None
    return _LOAD_OPERATION_BY_NODE.get(node_key)


def _operation_keys(operation: Any) -> Any:
    hash_value = safe_getattr(operation, "hash_value", None)
    if hash_value:
        return hash_value
    return safe_getattr(operation, "token_ids", None)


def _completed_tokens(operation: Any) -> int:
    value = _as_int(safe_getattr(operation, "completed_tokens", 0))
    return value if value is not None else 0


def _status_from_progress(completed_tokens: Optional[int], requested_tokens: Optional[int]) -> str:
    if requested_tokens is None or requested_tokens <= 0:
        return "ok"
    if completed_tokens is None:
        return "ok"
    if completed_tokens >= requested_tokens:
        return "ok"
    if completed_tokens > 0:
        return "partial"
    return "failed"


def _update_count_fields(event_args: Dict[str, Any], num_tokens: Optional[int] = None, num_pages: Optional[int] = None) -> None:
    if num_tokens is not None:
        event_args["num_tokens"] = num_tokens
    page_size = event_args.get("page_size")
    if num_pages is None:
        num_pages = _ceil_div(num_tokens, page_size)
    if num_pages is not None:
        event_args["num_pages"] = num_pages
    bytes_per_page = event_args.get("bytes_per_page")
    try:
        if num_pages is not None and bytes_per_page:
            event_args["bytes"] = int(num_pages) * int(bytes_per_page)
        elif num_pages == 0:
            event_args["bytes"] = 0
        else:
            event_args["bytes"] = None
    except Exception:
        event_args["bytes"] = None


def _add_probe_contract(event_args: Dict[str, Any], *, model_input: bool, event_kind: Optional[str] = None) -> Dict[str, Any]:
    event_args["schema_version"] = HICACHE_RADIX_SCHEMA
    event_args["model_input"] = bool(model_input)
    if event_kind is not None:
        event_args["event_kind"] = event_kind
    return event_args


def _missing_required(event_args: Dict[str, Any], required: Tuple[str, ...]) -> list[str]:
    missing = []
    for key in required:
        value = event_args.get(key)
        if value is None or value == "":
            missing.append(key)
    return missing


def _finalize_model_input(event_args: Dict[str, Any], required: Tuple[str, ...]) -> Dict[str, Any]:
    missing = _missing_required(event_args, required)
    if missing:
        event_args["model_input"] = False
        event_args["rejected_reason"] = "missing:" + ",".join(missing)
    else:
        event_args["model_input"] = True
    if event_args.get("model_input") and event_args.get("event_kind") == "storage_op":
        runtime_pages = _pipe_count(event_args.get("page_keys_hash"))
        block_pages = _pipe_count(event_args.get("trace_page_block_keys_hash"))
        if event_args.get("direction") == "query":
            expected_pages = _as_int(event_args.get("queried_pages"))
        else:
            expected_pages = _as_int(event_args.get("success_pages"))
            if expected_pages is None:
                expected_pages = _as_int(event_args.get("num_pages"))
        if runtime_pages != block_pages or (expected_pages is not None and block_pages != expected_pages):
            event_args["model_input"] = False
            event_args["rejected_reason"] = "page_identity_mismatch"
    event_args["schema_version"] = HICACHE_RADIX_SCHEMA
    return event_args


def _model_input_or_none(event_args: Dict[str, Any], required: Tuple[str, ...]) -> Optional[Dict[str, Any]]:
    missing = _missing_required(event_args, required)
    if missing:
        return None
    event_args["model_input"] = True
    event_args["schema_version"] = HICACHE_RADIX_SCHEMA
    return event_args


def _current_storage_context() -> Dict[str, Any]:
    context = getattr(_STORAGE_CONTEXT, "value", None)
    return dict(context) if isinstance(context, dict) else {}


def _set_storage_context(context: Optional[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
    previous = getattr(_STORAGE_CONTEXT, "value", None)
    _STORAGE_CONTEXT.value = dict(context) if isinstance(context, dict) else {}
    return previous


def _restore_storage_context(previous: Optional[Dict[str, Any]]) -> None:
    if previous is None:
        try:
            delattr(_STORAGE_CONTEXT, "value")
        except AttributeError:
            pass
    else:
        _STORAGE_CONTEXT.value = previous


def _current_operation_context() -> Dict[str, Any]:
    context = getattr(_OPERATION_CONTEXT, "value", None)
    return dict(context) if isinstance(context, dict) else {}


def _set_operation_context(context: Optional[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
    previous = getattr(_OPERATION_CONTEXT, "value", None)
    _OPERATION_CONTEXT.value = dict(context) if isinstance(context, dict) else {}
    return previous


def _restore_operation_context(previous: Optional[Dict[str, Any]]) -> None:
    if previous is None:
        try:
            delattr(_OPERATION_CONTEXT, "value")
        except AttributeError:
            pass
    else:
        _OPERATION_CONTEXT.value = previous


def _storage_context_from_args(event_args: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "request_id": event_args.get("request_id"),
        "operation_id": event_args.get("operation_id"),
        "page_size": event_args.get("page_size"),
        "bytes_per_page": event_args.get("bytes_per_page"),
        "storage_backend": event_args.get("storage_backend"),
        "cache_id": event_args.get("cache_id"),
        "page_identity_kind": event_args.get("page_identity_kind"),
        "full_path_block_keys_hash": event_args.get("full_path_block_keys_hash"),
        "trace_page_block_keys_hash": event_args.get("trace_page_block_keys_hash"),
    }


def _count_success_pages(result: Any, requested_pages: Optional[int] = None) -> Optional[int]:
    if result is None:
        return 0
    kv_hit_pages = _as_int(safe_getattr(result, "kv_hit_pages", None))
    if kv_hit_pages is not None:
        return max(0, kv_hit_pages)
    if isinstance(result, bool):
        return requested_pages if result and requested_pages is not None else int(result)
    if isinstance(result, int):
        return max(0, result)
    if isinstance(result, dict):
        total = 0
        found = False
        for value in result.values():
            count = _count_success_pages(value)
            if count is not None:
                total += count
                found = True
        return total if found else None
    try:
        values = list(result)
    except Exception:
        return None
    success = 0
    for value in values:
        if isinstance(value, bool):
            success += int(value)
        elif value is not None:
            success += 1
    return success


def _storage_keys_from_transfers(transfers: Any) -> Any:
    keys = []
    for transfer in _safe_iter(transfers):
        keys.extend(_safe_iter(safe_getattr(transfer, "keys", None)))
    return keys


def _storage_transfer_pages(transfers: Any) -> Optional[int]:
    total = 0
    found = False
    for transfer in _safe_iter(transfers):
        count = _tensor_count(safe_getattr(transfer, "keys", None))
        if count is not None:
            total += count
            found = True
    return total if found else None


def _storage_method_kind(method_name: str) -> Tuple[str, str, str, str, str]:
    if "exists" in method_name:
        return (HICACHE_STORAGE_OP, "L3", "L2", "query", "storage_op")
    if "get" in method_name:
        return (HICACHE_STORAGE_OP, "L3", "L2", "prefetch", "storage_op")
    if "set" in method_name:
        return (HICACHE_STORAGE_OP, "L2", "L3", "write", "storage_op")
    return (HICACHE_STORAGE_OP, "", "", "storage", "storage_op")


def _storage_base_args(self_obj: Any, method_name: str, args: Tuple[Any, ...], kwargs: Dict[str, Any]) -> Dict[str, Any]:
    context = _current_storage_context()
    event_name, tier_src, tier_dst, direction, event_kind = _storage_method_kind(method_name)
    keys = kwargs.get("keys")
    if keys is None and args:
        keys = args[0]
    transfers = kwargs.get("transfers") or kwargs.get("pool_transfers")
    if transfers is None and method_name in ("batch_get_v2", "batch_set_v2") and args:
        transfers = args[0]
    if transfers is None and method_name == "batch_exists_v2" and len(args) > 1:
        transfers = args[1]
    if transfers is not None:
        keys = _storage_keys_from_transfers(transfers)

    page_size = safe_getattr(self_obj, "page_size", None) or context.get("page_size")
    requested_pages = _tensor_count(keys)
    if requested_pages is None and transfers is not None:
        requested_pages = _storage_transfer_pages(transfers)
    bytes_per_page = _infer_bytes_per_page(self_obj, page_size) or context.get("bytes_per_page")
    num_tokens = int(requested_pages) * int(page_size) if requested_pages is not None and page_size else None
    page_keys_hash, key_truncated = _encode_keys(keys, page_size)
    bytes_moved = int(requested_pages) * int(bytes_per_page) if requested_pages and bytes_per_page else None
    return {
        "schema_version": HICACHE_RADIX_SCHEMA,
        "model_input": False,
        "framework": FRAMEWORK_SGLANG,
        "producer": "python_probe",
        "domain": "cache_io",
        "event_kind": "storage_op",
        "debug_event_kind": event_kind,
        "method": method_name,
        "python_module": type(self_obj).__module__,
        "python_class": type(self_obj).__name__,
        "python_method": method_name,
        "request_id": _id_text(context.get("request_id")),
        "operation_id": _id_text(context.get("operation_id")),
        "op_id": compact_id(self_obj),
        "cache_id": _id_text(context.get("cache_id")),
        "tier_src": tier_src,
        "tier_dst": tier_dst,
        "direction": direction,
        "num_tokens": num_tokens,
        "num_pages": requested_pages,
        "queried_pages": requested_pages if "exists" in method_name else None,
        "page_size": page_size,
        "page_identity_kind": context.get("page_identity_kind") or "block_tuple",
        "full_path_block_keys_hash": context.get("full_path_block_keys_hash"),
        "trace_page_block_keys_hash": context.get("trace_page_block_keys_hash"),
        "bytes_per_page": bytes_per_page,
        "bytes": bytes_moved,
        "page_keys_hash": page_keys_hash,
        "runtime_page_keys_hash": page_keys_hash,
        "key_truncated": key_truncated,
        "storage_backend": type(self_obj).__name__,
        "pool": "storage",
        "pool_name": "KV",
        "_event_name": event_name,
    }


def _storage_wrapper(method_name: str):
    def make(original):
        def wrapped(self_obj, *args, **kwargs):
            event_args = _storage_base_args(self_obj, method_name, args, kwargs)
            event_name = event_args.pop("_event_name")
            writer = get_writer()
            start = writer.now_us()
            try:
                result = original(self_obj, *args, **kwargs)
                requested_pages = _as_int(event_args.get("num_pages"))
                success_pages = _count_success_pages(result, requested_pages)
                if "exists" in method_name:
                    event_args["hit_pages"] = success_pages
                    event_args["success_pages"] = success_pages
                    event_args["miss_pages"] = max(0, requested_pages - success_pages) if success_pages is not None else None
                    event_args["storage_hit_tokens"] = int(success_pages) * int(event_args.get("page_size")) if success_pages is not None and event_args.get("page_size") else None
                    if success_pages is None:
                        event_args["status"] = "ok"
                    elif success_pages <= 0:
                        event_args["status"] = "miss"
                    elif requested_pages and success_pages < requested_pages:
                        event_args["status"] = "partial"
                    else:
                        event_args["status"] = "hit"
                else:
                    event_args["success_pages"] = success_pages
                    event_args["storage_success_pages"] = success_pages
                    event_args["hit_pages"] = success_pages
                    event_args["miss_pages"] = max(0, requested_pages - success_pages) if success_pages is not None else None
                    if success_pages is not None:
                        event_args["page_keys_hash"] = _truncate_pipe(event_args.get("page_keys_hash"), success_pages)
                        event_args["runtime_page_keys_hash"] = _truncate_pipe(event_args.get("runtime_page_keys_hash"), success_pages)
                        event_args["trace_page_block_keys_hash"] = _truncate_pipe(event_args.get("trace_page_block_keys_hash"), success_pages)
                        tokens = int(success_pages) * int(event_args.get("page_size")) if event_args.get("page_size") else None
                        _update_count_fields(event_args, tokens, success_pages)
                    if success_pages is None:
                        event_args["status"] = "ok"
                    elif success_pages <= 0:
                        event_args["status"] = "failed"
                    elif requested_pages and success_pages < requested_pages:
                        event_args["status"] = "partial"
                    else:
                        event_args["status"] = "completed"
                return result
            except Exception as exc:
                event_args["status"] = "exception"
                event_args["exception"] = type(exc).__name__
                raise
            finally:
                _finalize_model_input(
                    event_args,
                    (
                        "operation_id",
                        "method",
                        "page_size",
                        "page_identity_kind",
                        "page_keys_hash",
                        "trace_page_block_keys_hash",
                        "num_pages",
                        "status",
                    ),
                )
                writer.duration_event(event_name, start, writer.now_us(), HICACHE_CATEGORY, event_args)

        return wrapped

    return make


def _aligned_token_len(raw_token_len: Optional[int], page_size: Any) -> Optional[int]:
    raw = _as_int(raw_token_len)
    page = _as_int(page_size)
    if raw is None or page is None or page <= 0:
        return None
    return raw // page * page


def _radix_inputs(self_obj: Any, method_name: str, args: Tuple[Any, ...], kwargs: Dict[str, Any], result: Any = None) -> Dict[str, Any]:
    node = None
    raw_keys = None
    node_local_raw_keys = None
    full_raw_keys = None
    parent_full_raw_keys = None
    raw_token_len = None
    parent_node_id = None
    node_id = None
    hit_count = None
    backuped = None
    evicted = None

    if method_name in ("write_backup", "write_backup_storage", "load_back", "_evict_backuped", "_evict_regular"):
        node = _arg(args, 0, kwargs, "node")
        raw_keys = _node_raw_keys(node)
        raw_token_len = _node_token_count(node)
    elif method_name == "insert":
        params = _arg(args, 0, kwargs, "params")
        key = safe_getattr(params, "key", None)
        value = safe_getattr(params, "value", None)
        raw_keys = safe_getattr(key, "token_ids", None) or key
        raw_token_len = _tensor_count(value) or _tensor_count(key)
    elif method_name == "match_prefix":
        params = _arg(args, 0, kwargs, "params")
        key = safe_getattr(params, "key", None)
        raw_keys = safe_getattr(key, "token_ids", None) or key
        raw_token_len = _tensor_count(key)
        node = safe_getattr(result, "last_device_node", None) or safe_getattr(result, "last_host_node", None)
    elif method_name == "prefetch_from_storage":
        raw_keys = _arg(args, 2, kwargs, "new_input_tokens")
        raw_token_len = _tensor_count(raw_keys)
        node = _arg(args, 1, kwargs, "last_host_node")
    elif method_name == "_insert_helper_host":
        node = _arg(args, 0, kwargs, "node")
        key = _arg(args, 1, kwargs, "key")
        raw_keys = safe_getattr(key, "token_ids", None) or key
        raw_token_len = _tensor_count(key)
    elif method_name in ("write_storage",):
        token_ids = _arg(args, 1, kwargs, "token_ids")
        raw_keys = safe_getattr(token_ids, "token_ids", None) or token_ids
        raw_token_len = _tensor_count(token_ids)
    elif method_name in ("prefetch",):
        raw_keys = _arg(args, 2, kwargs, "new_input_tokens")
        raw_token_len = _tensor_count(raw_keys)
    elif method_name in ("_storage_hit_query", "_page_transfer", "_page_backup", "terminate_prefetch"):
        operation = _arg(args, 0, kwargs, "operation")
        token_ids = safe_getattr(operation, "token_ids", None)
        raw_keys = safe_getattr(token_ids, "token_ids", None) or token_ids
        raw_token_len = _tensor_count(token_ids)

    if node is not None:
        node_id = _node_id(node)
        parent_node_id = _node_parent_id(node)
        node_local_raw_keys = _node_raw_keys(node)
        full_raw_keys = _node_full_raw_keys(node)
        parent_full_raw_keys = _node_full_raw_keys(_node_parent(node))
        hit_count = _as_int(safe_getattr(node, "hit_count", None))
        backuped = _bool_attr(node, "backuped")
        evicted = _bool_attr(node, "evicted")
    if full_raw_keys is None:
        full_raw_keys = raw_keys

    return {
        "raw_keys": raw_keys,
        "node_local_raw_keys": node_local_raw_keys,
        "full_raw_keys": full_raw_keys,
        "parent_full_raw_keys": parent_full_raw_keys,
        "raw_token_len": raw_token_len,
        "node_id": node_id,
        "parent_node_id": parent_node_id,
        "hit_count": hit_count,
        "backuped": backuped,
        "evicted": evicted,
    }


def _radix_op_args(
    base_args: Dict[str, Any],
    self_obj: Any,
    method_name: str,
    args: Tuple[Any, ...],
    kwargs: Dict[str, Any],
    result: Any = None,
) -> Optional[Dict[str, Any]]:
    if type(self_obj).__name__ != "HiRadixCache":
        return None
    if method_name not in RADIX_MODEL_METHODS:
        return None

    radix = _radix_inputs(self_obj, method_name, args, kwargs, result)
    raw_token_len = _as_int(radix.get("raw_token_len"))
    page_size = base_args.get("page_size")
    aligned_len = _aligned_token_len(raw_token_len, page_size)
    dropped = (raw_token_len - aligned_len) if raw_token_len is not None and aligned_len is not None else None
    node_local_block_keys_hash, node_local_truncated, block_size = _encode_block_keys(
        radix.get("node_local_raw_keys")
    )
    full_path_block_keys_hash, full_path_truncated, block_size = _encode_block_keys(radix.get("full_raw_keys"))
    parent_full_path_block_keys_hash, parent_full_truncated, block_size = _encode_block_keys(radix.get("parent_full_raw_keys"))
    trace_page_block_keys_hash, trace_page_truncated, block_size = _encode_trace_page_block_keys(radix.get("full_raw_keys"), page_size)
    page = _as_int(page_size)
    warning = None
    if page and page % block_size != 0:
        warning = "page_size_not_divisible_by_block_size"
    node_id = _id_text(radix.get("node_id")) or _id_text(base_args.get("node_id"))
    if node_id is None and full_path_block_keys_hash:
        node_id = f"path:{full_path_block_keys_hash}"

    event_args = dict(base_args)
    event_args.update(
        {
            "event_kind": "radix_op",
            "model_input": True,
            "schema_version": HICACHE_RADIX_SCHEMA,
            "method": method_name,
            "op_seq": next(_OP_SEQUENCE),
            "cache_id": compact_id(self_obj),
            "node_id": node_id,
            "raw_token_len": raw_token_len,
            "aligned_token_len": aligned_len,
            "dropped_tail_tokens": dropped,
            "parent_node_id": _id_text(radix.get("parent_node_id")),
            "node_key_len": raw_token_len,
            "hit_count": radix.get("hit_count"),
            "backuped": radix.get("backuped"),
            "evicted": radix.get("evicted"),
            "page_identity_kind": "block_tuple",
            "block_keys_hash": full_path_block_keys_hash,
            "node_local_block_keys_hash": node_local_block_keys_hash,
            "full_path_block_keys_hash": full_path_block_keys_hash,
            "parent_full_path_block_keys_hash": parent_full_path_block_keys_hash,
            "trace_page_block_keys_hash": trace_page_block_keys_hash,
            "block_key_truncated": bool(node_local_truncated or full_path_truncated or parent_full_truncated or trace_page_truncated),
            "block_size_tokens": block_size,
            "radix_warning": warning,
        }
    )
    required = [
        "op_seq",
        "cache_id",
        "method",
        "block_size_tokens",
        "page_size",
    ]
    if method_name not in ("evict", "evict_host"):
        required.extend(("node_id", "full_path_block_keys_hash", "trace_page_block_keys_hash", "raw_token_len", "aligned_token_len"))
    return _model_input_or_none(event_args, tuple(required))


def _last_queue_operation(self_obj: Any, queue_name: str) -> Any:
    queue = safe_getattr(self_obj, queue_name, None)
    try:
        return queue[-1]
    except Exception:
        return None


def _cache_operation_kind_stage(method_name: str, status: str) -> Tuple[Optional[str], Optional[str]]:
    if method_name == "prefetch":
        return "prefetch", "created" if status == "ok" else "failed"
    if method_name in ("_storage_hit_query",):
        return "prefetch", "started" if status == "ok" else "failed"
    if method_name in ("_insert_helper_host", "terminate_prefetch"):
        return "prefetch", "completed" if status == "ok" else "failed"
    if method_name in ("load", "init_load_back"):
        return "load", "queued" if status == "ok" else "failed"
    if method_name == "load_back":
        return "load", "completed" if status == "ok" else "failed"
    if method_name in ("write", "write_storage"):
        return "write", "queued" if status == "ok" else "failed"
    return None, None


def _cache_operation_args(
    base_args: Dict[str, Any],
    self_obj: Any,
    method_name: str,
    args: Tuple[Any, ...],
    kwargs: Dict[str, Any],
    result: Any = None,
) -> Optional[Dict[str, Any]]:
    if method_name not in CACHE_OPERATION_METHODS:
        return None
    status = str(base_args.get("status") or "ok")
    operation_kind, stage = _cache_operation_kind_stage(method_name, status)
    if operation_kind is None or stage is None:
        return None

    radix = _radix_inputs(self_obj, method_name, args, kwargs, result)
    full_path_block_keys_hash, _, block_size = _encode_block_keys(radix.get("full_raw_keys"))
    trace_page_block_keys_hash, _, block_size = _encode_trace_page_block_keys(radix.get("full_raw_keys"), base_args.get("page_size"))
    operation_id = _id_text(base_args.get("operation_id"))
    node_id = _id_text(base_args.get("node_id")) or _id_text(radix.get("node_id"))
    if operation_id is None and method_name == "load_back":
        operation_id = _load_operation_for_node(node_id)
    if node_id is None and not full_path_block_keys_hash and not base_args.get("page_keys_hash"):
        return None

    event_args = dict(base_args)
    event_args.update(
        {
            "event_kind": "cache_operation",
            "model_input": True,
            "schema_version": HICACHE_RADIX_SCHEMA,
            "method": method_name,
            "operation_id": operation_id,
            "operation_kind": operation_kind,
            "stage": stage,
            "node_id": node_id,
            "page_identity_kind": "block_tuple",
            "full_path_block_keys_hash": full_path_block_keys_hash,
            "trace_page_block_keys_hash": trace_page_block_keys_hash,
            "block_size_tokens": block_size,
        }
    )
    return _model_input_or_none(
        event_args,
        (
            "operation_id",
            "operation_kind",
            "stage",
            "status",
        ),
    )


def _base_args(self_obj: Any, method_name: str, args: Tuple[Any, ...], kwargs: Dict[str, Any]) -> Dict[str, Any]:
    context = _current_operation_context()
    cache_id = _cache_id_for_object(self_obj)
    page_size = safe_getattr(self_obj, "page_size", None)
    storage_backend = safe_getattr(self_obj, "storage_backend_type", None)
    storage_obj = safe_getattr(self_obj, "storage_backend", None)
    if storage_backend is None and storage_obj is not None:
        storage_backend = type(storage_obj).__name__

    request_id = (
        context.get("request_id")
        or kwargs.get("req_id")
        or kwargs.get("request_id")
        or (_arg(args, 0, kwargs, "req_id") if method_name in ("check_prefetch_progress", "terminate_prefetch", "pop_prefetch_loaded_tokens") else None)
        or (_arg(args, 0, kwargs, "request_id") if method_name == "prefetch" else None)
    )

    host_indices = kwargs.get("host_indices")
    device_indices = kwargs.get("device_indices")
    token_ids = kwargs.get("token_ids") or kwargs.get("new_input_tokens")
    page_keys = None
    node_id = None
    operation_id = context.get("operation_id")
    block_identity_keys = None
    full_path_block_keys_hash = context.get("full_path_block_keys_hash")
    trace_page_block_keys_hash = context.get("trace_page_block_keys_hash")
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
        block_identity_keys = token_ids
    elif method_name == "prefetch_from_storage":
        token_ids = _arg(args, 2, kwargs, "new_input_tokens")
        block_identity_keys = token_ids
    elif method_name == "write_storage":
        host_indices = _arg(args, 0, kwargs, "host_indices")
        token_ids = _arg(args, 1, kwargs, "token_ids")
        page_keys = _arg(args, 2, kwargs, "hash_value") or token_ids
        block_identity_keys = token_ids
    elif method_name == "start_loading":
        operation_summary = _summarize_operations(safe_getattr(self_obj, "load_queue", None), "host_indices", "device_indices")
        if operation_summary.get("num_tokens") is not None:
            inferred_num_tokens = operation_summary["num_tokens"]
        operation_id = operation_summary.get("operation_id")
        node_id = operation_summary.get("node_id")
        _remember_load_operations(operation_summary)
        pool_transfers = operation_summary.get("pool_transfers") or pool_transfers
    elif method_name == "start_writing":
        operation_summary = _summarize_operations(safe_getattr(self_obj, "write_queue", None), "device_indices", "host_indices")
        if operation_summary.get("num_tokens") is not None:
            inferred_num_tokens = operation_summary["num_tokens"]
        operation_id = operation_summary.get("operation_id")
        node_id = operation_summary.get("node_id")
        pool_transfers = operation_summary.get("pool_transfers") or pool_transfers
    elif method_name in ("_storage_hit_query", "_page_transfer", "_page_backup", "terminate_prefetch"):
        operation = _arg(args, 0, kwargs, "operation")
        operation_id = safe_getattr(operation, "id", None)
        host_indices = safe_getattr(operation, "host_indices", None)
        token_ids = safe_getattr(operation, "token_ids", None) or safe_getattr(operation, "hash_value", None)
        request_id = request_id or safe_getattr(operation, "request_id", None)
        page_keys = _operation_keys(operation)
        block_identity_keys = safe_getattr(operation, "token_ids", None)
        pool_transfers = safe_getattr(operation, "pool_transfers", None)
    elif method_name in ("_page_get_zero_copy", "_generic_page_get"):
        operation = _arg(args, 0, kwargs, "operation")
        hash_values = _arg(args, 1, kwargs, "hash_values")
        host_indices = _arg(args, 2, kwargs, "host_indices")
        operation_id = safe_getattr(operation, "id", None)
        request_id = safe_getattr(operation, "request_id", None)
        page_keys = hash_values
        block_identity_keys = safe_getattr(operation, "token_ids", None)
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
            block_identity_keys = safe_getattr(operation, "token_ids", None)
    elif method_name in ("write_backup", "write_backup_storage", "load_back", "_evict_backuped", "_evict_regular"):
        node = _arg(args, 0, kwargs, "node")
        node_id = _node_id(node)
        page_keys = _node_keys(node)
        block_identity_keys = _node_full_raw_keys(node)
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
        block_identity_keys = safe_getattr(key, "token_ids", None) or key
        inferred_num_tokens = _tensor_count(host_indices) or _tensor_count(key)
    elif method_name == "insert":
        params = _arg(args, 0, kwargs, "params")
        key = safe_getattr(params, "key", None)
        value = safe_getattr(params, "value", None)
        page_keys = safe_getattr(key, "token_ids", None) or key
        block_identity_keys = page_keys
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
    if block_identity_keys is not None:
        full_path_block_keys_hash, _, _ = _encode_block_keys(block_identity_keys)
        trace_page_block_keys_hash, _, _ = _encode_trace_page_block_keys(block_identity_keys, page_size)
    pool_name, pool_transfer_count = _pool_transfer_names(pool_transfers)
    if pool_transfer_count:
        transfer_scope = "kv+extra_pool"
    return {
        "schema_version": HICACHE_RADIX_SCHEMA,
        "model_input": False,
        "framework": FRAMEWORK_SGLANG,
        "producer": "python_probe",
        "domain": "cache_io",
        "event_kind": event_kind,
        "python_module": type(self_obj).__module__,
        "python_class": type(self_obj).__name__,
        "python_method": method_name,
        "op_id": compact_id(self_obj),
        "cache_id": _id_text(cache_id),
        "operation_id": _id_text(operation_id),
        "node_id": _id_text(node_id),
        "request_id": _id_text(request_id),
        "tier_src": tier_src,
        "tier_dst": tier_dst,
        "direction": direction,
        "num_tokens": num_tokens,
        "num_pages": num_pages,
        "page_size": page_size,
        "page_identity_kind": "block_tuple",
        "full_path_block_keys_hash": full_path_block_keys_hash,
        "trace_page_block_keys_hash": trace_page_block_keys_hash,
        "bytes_per_page": bytes_per_page,
        "bytes": bytes_moved,
        "page_keys_hash": page_keys_hash,
        "runtime_page_keys_hash": page_keys_hash,
        "key_truncated": key_truncated,
        "io_backend": safe_getattr(self_obj, "io_backend", None),
        "storage_backend": storage_backend,
        "write_policy": safe_getattr(self_obj, "write_policy", None),
        "prefetch_threshold": safe_getattr(self_obj, "prefetch_threshold", None),
        "prefetch_capacity_limit": safe_getattr(self_obj, "prefetch_capacity_limit", None),
        "storage_batch_size": safe_getattr(self_obj, "storage_batch_size", None),
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
            operation = _arg(args, 0, kwargs, "operation") if method_name in (
                "_storage_hit_query",
                "_page_transfer",
                "_page_backup",
                "_page_get_zero_copy",
                "_generic_page_get",
            ) else None
            if operation is not None:
                if event_args.get("operation_id") in (None, ""):
                    event_args["operation_id"] = _id_text(safe_getattr(operation, "id", None))
                if event_args.get("request_id") in (None, ""):
                    event_args["request_id"] = _id_text(safe_getattr(operation, "request_id", None))
            completed_before = _completed_tokens(operation) if operation is not None else None
            writer = get_writer()
            start = writer.now_us()
            previous_storage_context = None
            storage_context_active = False
            previous_operation_context = None
            operation_context_active = False
            if method_name == "check_prefetch_progress":
                req_id = _arg(args, 0, kwargs, "req_id")
                operation_for_context = _prefetch_operation_for(self_obj, req_id)
                previous_operation_context = _set_operation_context(
                    {
                        "request_id": _id_text(req_id),
                        "operation_id": _id_text(safe_getattr(operation_for_context, "id", None)),
                    }
                )
                operation_context_active = True
            if method_name in (
                "_storage_hit_query",
                "_page_transfer",
                "_page_get_zero_copy",
                "_generic_page_get",
                "_draft_page_get",
                "_page_backup",
                "_page_set_zero_copy",
                "_generic_page_set",
                "_draft_page_set",
            ):
                previous_storage_context = _set_storage_context(_storage_context_from_args(event_args))
                storage_context_active = True
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
                elif method_name == "prefetch":
                    event_args["operation_id"] = _id_text(safe_getattr(result, "id", event_args.get("operation_id")))
                    if result is None:
                        event_args["status"] = "failed"
                elif method_name == "write_storage":
                    event_args["operation_id"] = _id_text(result)
                    if result is None:
                        event_args["status"] = "failed"
                elif method_name in ("load", "write"):
                    queue_name = "load_queue" if method_name == "load" else "write_queue"
                    queued_op = _last_queue_operation(self_obj, queue_name)
                    event_args["operation_id"] = _id_text(safe_getattr(queued_op, "id", event_args.get("operation_id")))
                    if event_args.get("node_id") in (None, ""):
                        event_args["node_id"] = _join_values(safe_getattr(queued_op, "node_ids", None))
                    if method_name == "load":
                        _remember_load_operations(
                            {
                                "operation_id": event_args.get("operation_id"),
                                "node_id": event_args.get("node_id"),
                            }
                        )
                    if result is None:
                        event_args["status"] = "failed"
                elif method_name in ("_page_transfer", "_page_get_zero_copy", "_generic_page_get"):
                    if operation is not None and completed_before is not None:
                        completed_delta = max(0, _completed_tokens(operation) - completed_before)
                        event_args["completed_tokens_delta"] = completed_delta
                        requested_tokens = _as_int(event_args.get("num_tokens"))
                        event_args["status"] = _status_from_progress(completed_delta, requested_tokens)
                        _update_count_fields(event_args, completed_delta)
                elif method_name == "_page_backup":
                    if operation is not None and completed_before is not None:
                        completed_delta = max(0, _completed_tokens(operation) - completed_before)
                        event_args["completed_tokens_delta"] = completed_delta
                        requested_tokens = _as_int(event_args.get("num_tokens"))
                        event_args["status"] = _status_from_progress(completed_delta, requested_tokens)
                        _update_count_fields(event_args, completed_delta)
                elif method_name in ("_page_set_zero_copy", "_generic_page_set"):
                    success = bool(result)
                    event_args["storage_success"] = success
                    event_args["status"] = "ok" if success else "failed"
                    if not success:
                        _update_count_fields(event_args, 0, 0)
                elif method_name in ("_draft_page_get", "_draft_page_set"):
                    event_args["status"] = "best_effort"
                elif method_name == "load_back" and event_args.get("operation_id") in (None, ""):
                    event_args["operation_id"] = _load_operation_for_node(event_args.get("node_id"))
                return result
            except Exception as exc:
                event_args["status"] = "exception"
                event_args["exception"] = type(exc).__name__
                raise
            finally:
                if storage_context_active:
                    _restore_storage_context(previous_storage_context)
                if operation_context_active:
                    _restore_operation_context(previous_operation_context)
                event_args["model_input"] = False
                writer.duration_event(event_name, start, writer.now_us(), HICACHE_CATEGORY, event_args)
                radix_args = _radix_op_args(event_args, self_obj, method_name, args, kwargs, locals().get("result"))
                if radix_args is not None:
                    writer.duration_event(HICACHE_RADIX_OP, start, writer.now_us(), HICACHE_CATEGORY, radix_args)
                cache_operation_args = _cache_operation_args(event_args, self_obj, method_name, args, kwargs, locals().get("result"))
                if cache_operation_args is not None:
                    writer.duration_event(HICACHE_CACHE_OPERATION, start, writer.now_us(), HICACHE_CATEGORY, cache_operation_args)
                pool_storage_result = safe_getattr(operation, "pool_storage_result", None) if operation is not None else None
                for pool_event_args in _extra_pool_event_args(event_args, pool_transfers, pool_storage_result):
                    pool_event_args["schema_version"] = HICACHE_RADIX_SCHEMA
                    pool_event_args["model_input"] = False
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
    for _, cls in vars(module).items():
        if not isinstance(cls, type):
            continue
        if not any(hasattr(cls, method_name) for method_name in STORAGE_METHODS):
            continue
        for method_name in STORAGE_METHODS:
            changed = wrap_method(cls, method_name, _storage_wrapper(method_name)) or changed
    return changed
