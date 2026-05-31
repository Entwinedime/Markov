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
    "check_prefetch_progress": (HICACHE_PREFETCH_L3_TO_L2, "L3", "L2", "prefetch"),
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
    "flush_write_through_acks": (HICACHE_BACKUP_L1_TO_L2, "L1", "L2", "backup"),
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


def _arg(args: Tuple[Any, ...], index: int, kwargs: Dict[str, Any], name: str) -> Any:
    if name in kwargs:
        return kwargs[name]
    if index < len(args):
        return args[index]
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

    num_tokens = _first_count(host_indices, device_indices, token_ids)
    num_pages = None
    try:
        if num_tokens is not None and page_size:
            num_pages = (int(num_tokens) + int(page_size) - 1) // int(page_size)
    except Exception:
        num_pages = None

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
