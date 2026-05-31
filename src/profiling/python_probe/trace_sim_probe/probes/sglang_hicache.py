from __future__ import annotations

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
    "sglang.srt.managers.cache_controller",
    "sglang.srt.mem_cache.hybrid_cache.hybrid_cache_controller",
)


METHOD_EVENTS = {
    "match_prefix": (HICACHE_MATCH, "", "", "lookup"),
    "prefetch_from_storage": (HICACHE_PREFETCH_QUERY, "L3", "L2", "prefetch"),
    "prefetch": (HICACHE_PREFETCH_QUERY, "L3", "L2", "prefetch"),
    "_storage_hit_query": (HICACHE_PREFETCH_QUERY, "L3", "L2", "query"),
    "_page_transfer": (HICACHE_PREFETCH_L3_TO_L2, "L3", "L2", "prefetch"),
    "check_prefetch_progress": ("HiCache::prefetch_progress", "", "", "control"),
    "load": (HICACHE_LOAD_L2_TO_L1, "L2", "L1", "load"),
    "load_back": (HICACHE_LOAD_L2_TO_L1, "L2", "L1", "load"),
    "start_loading": (HICACHE_LOAD_L2_TO_L1, "L2", "L1", "load"),
    "ready_to_load_host_cache": (HICACHE_LOAD_L2_TO_L1, "L2", "L1", "load"),
    "write": (HICACHE_BACKUP_L1_TO_L2, "L1", "L2", "backup"),
    "start_writing": (HICACHE_BACKUP_L1_TO_L2, "L1", "L2", "backup"),
    "write_storage": (HICACHE_WRITE_L2_TO_L3, "L2", "L3", "write"),
    "_page_backup": (HICACHE_WRITE_L2_TO_L3, "L2", "L3", "write"),
    "backup_thread_func": (HICACHE_WRITE_L2_TO_L3, "L2", "L3", "write"),
    "evict_device": (HICACHE_EVICT_L1, "L1", "", "evict"),
    "evict_host": (HICACHE_EVICT_L2, "L2", "", "evict"),
    "evict": (HICACHE_EVICT_L1, "L1", "", "evict"),
    "insert": ("HiCache::insert_l1", "", "L1", "insert"),
    "init_load_back": (HICACHE_LOAD_L2_TO_L1, "L2", "L1", "load"),
    "terminate_prefetch": (HICACHE_PREFETCH_L3_TO_L2, "L3", "L2", "prefetch"),
    "pop_prefetch_loaded_tokens": (HICACHE_PREFETCH_L3_TO_L2, "L3", "L2", "prefetch"),
    "append_host_mem_release": (HICACHE_EVICT_L2, "L2", "", "release"),
    "attach_storage_backend": ("HiCache::attach_storage_backend", "", "L3", "control"),
    "detach_storage_backend": ("HiCache::detach_storage_backend", "L3", "", "control"),
    "check_hicache_events": ("HiCache::check_events", "", "", "control"),
    "flush_write_through_acks": ("HiCache::flush_write_acks", "", "", "control"),
}


CLASS_METHODS = {
    "HiRadixCache": (
        "match_prefix",
        "prefetch_from_storage",
        "check_prefetch_progress",
        "load_back",
        "ready_to_load_host_cache",
        "flush_write_through_acks",
        "check_hicache_events",
        "evict",
        "evict_host",
        "insert",
        "init_load_back",
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
        "_storage_hit_query",
        "_page_transfer",
        "prefetch_rate_limited",
        "write_storage",
        "_page_backup",
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
        "_storage_hit_query",
        "_page_transfer",
        "write_storage",
        "_page_backup",
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
        host_indices = safe_getattr(operation, "host_indices", None)
        token_ids = safe_getattr(operation, "token_ids", None) or safe_getattr(operation, "hash_value", None)
        request_id = request_id or safe_getattr(operation, "request_id", None)
    elif method_name == "check_prefetch_progress":
        operation = _prefetch_operation_for(self_obj, request_id)
        if operation is not None:
            host_indices = safe_getattr(operation, "host_indices", None)
            token_ids = safe_getattr(operation, "hash_value", None)

    num_tokens = inferred_num_tokens if inferred_num_tokens is not None else _first_count(host_indices, device_indices, token_ids)
    num_pages = None
    try:
        if num_tokens is not None and page_size:
            num_pages = (int(num_tokens) + int(page_size) - 1) // int(page_size)
    except Exception:
        num_pages = None
    bytes_per_page = _infer_bytes_per_page(self_obj, page_size)
    bytes_moved = num_pages * bytes_per_page if num_pages and bytes_per_page else None

    event_name, tier_src, tier_dst, direction = METHOD_EVENTS.get(method_name, (f"HiCache::{method_name}", "", "", "runtime"))
    return {
        "framework": FRAMEWORK_SGLANG,
        "producer": "python_probe",
        "domain": "cache_io",
        "python_module": type(self_obj).__module__,
        "python_class": type(self_obj).__name__,
        "python_method": method_name,
        "op_id": compact_id(self_obj),
        "request_id": request_id,
        "tier_src": tier_src,
        "tier_dst": tier_dst,
        "direction": direction,
        "num_tokens": num_tokens,
        "num_pages": num_pages,
        "page_size": page_size,
        "bytes_per_page": bytes_per_page,
        "bytes": bytes_moved,
        "io_backend": safe_getattr(self_obj, "io_backend", None),
        "storage_backend": storage_backend,
        "write_policy": safe_getattr(self_obj, "write_policy", None),
        "pool": type(safe_getattr(self_obj, "mem_pool_host", None)).__name__ if safe_getattr(self_obj, "mem_pool_host", None) is not None else None,
        "_event_name": event_name,
    }


def _wrapper(method_name: str):
    def make(original):
        def wrapped(self_obj, *args, **kwargs):
            event_args = _base_args(self_obj, method_name, args, kwargs)
            event_name = event_args.pop("_event_name")
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
                return result
            except Exception as exc:
                event_args["status"] = "exception"
                event_args["exception"] = type(exc).__name__
                raise
            finally:
                writer.duration_event(event_name, start, writer.now_us(), HICACHE_CATEGORY, event_args)

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
