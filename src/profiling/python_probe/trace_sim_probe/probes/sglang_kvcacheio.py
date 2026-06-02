from __future__ import annotations

from typing import Any, Dict, Optional, Tuple

from trace_sim_probe.patching import safe_len, wrap_function
from trace_sim_probe.schema import (
    FRAMEWORK_SGLANG,
    HICACHE_CATEGORY,
    HICACHE_TRANSFER_KV_DIM_EXCHANGE,
)
from trace_sim_probe.writer import get_writer


TARGET_MODULES = (
    "sgl_kernel.kvcacheio",
    "sgl_kernel_npu.kvcacheio",
    "sglang.srt.mem_cache.memory_pool_host",
)


TRANSFER_FUNCTIONS = (
    "transfer_kv_per_layer",
    "transfer_kv_per_layer_pf_lf",
    "transfer_kv_per_layer_ph_lf",
    "transfer_kv_all_layer",
    "transfer_kv_all_layer_lf_pf",
    "transfer_kv_all_layer_lf_ph",
    "transfer_kv_direct",
    "transfer_kv_per_layer_direct_pf_lf",
    "transfer_kv_all_layer_direct_lf_pf",
    "transfer_kv_per_layer_mla",
    "transfer_kv_per_layer_mla_pf_lf",
    "transfer_kv_all_layer_mla",
    "transfer_kv_all_layer_mla_lf_pf",
    "transfer_kv_dim_exchange",
    "jit_transfer_hicache_one_layer",
    "jit_transfer_hicache_all_layer",
    "jit_transfer_hicache_one_layer_mla",
    "jit_transfer_hicache_all_layer_mla",
)


def _arg(args: Tuple[Any, ...], index: int, kwargs: Dict[str, Any], name: str) -> Any:
    if name in kwargs:
        return kwargs[name]
    if index < len(args):
        return args[index]
    return None


def _layout_from_name(name: str) -> str:
    if name.startswith("jit_transfer_hicache"):
        return "jit"
    if "_pf_" in name:
        return "page_first"
    if "_ph_" in name:
        return "page_head"
    if "_lf_" in name:
        return "layer_first"
    if "_direct" in name:
        return "direct"
    if "_mla" in name:
        return "mla"
    return "unknown"


def _as_int(value: Any) -> Optional[int]:
    if value is None:
        return None
    try:
        return int(value)
    except Exception:
        return None


def _ceil_div(value: Any, divisor: Any) -> Optional[int]:
    numerator = _as_int(value)
    denominator = _as_int(divisor)
    if numerator is None or denominator is None or denominator <= 0:
        return None
    return (numerator + denominator - 1) // denominator


def _direction_text(value: Any) -> str:
    if value is None:
        return ""
    name = getattr(value, "name", None)
    if name is not None:
        return str(name)
    text = str(value)
    return text.rsplit(".", 1)[-1]


def _tiers_from_direction(value: Any) -> tuple[str, str, str]:
    text = _direction_text(value).upper()
    if text == "H2D":
        return "L2", "L1", "load"
    if text == "D2H":
        return "L1", "L2", "backup"
    return "", "", "transfer"


def _tiers_from_function_name(function_name: str) -> tuple[str, str, str]:
    if function_name.startswith("jit_transfer_hicache_one_layer"):
        return "L2", "L1", "load"
    if function_name.startswith("jit_transfer_hicache_all_layer"):
        return "L1", "L2", "backup"
    if function_name.startswith("transfer_kv_per_layer"):
        return "L2", "L1", "load"
    if function_name.startswith("transfer_kv_all_layer"):
        return "L1", "L2", "backup"
    return "", "", "transfer"


def _wrapper(function_name: str):
    def make(original):
        def wrapped(*args, **kwargs):
            src_indices = kwargs.get("src_indices")
            if src_indices is None:
                src_indices = kwargs.get("indices_src")
            dst_indices = kwargs.get("dst_indices")
            if dst_indices is None:
                dst_indices = kwargs.get("indices_dst")
            if src_indices is None:
                src_indices = _arg(args, 4, kwargs, "src_indices")
            if dst_indices is None:
                dst_indices = _arg(args, 5, kwargs, "dst_indices")
            page_size = kwargs.get("page_size")
            if page_size is None and function_name.endswith(("_ph_lf", "_lf_ph", "_direct", "_direct_pf_lf", "_direct_lf_pf")):
                page_size = args[-1] if args else None
            transfer_direction = kwargs.get("direction")
            tier_src, tier_dst, direction = _tiers_from_direction(transfer_direction)
            if not tier_src and not tier_dst:
                tier_src, tier_dst, direction = _tiers_from_function_name(function_name)

            count = safe_len(src_indices) or safe_len(dst_indices)
            num_pages = _ceil_div(count, page_size) if page_size is not None else count
            event_args = {
                "framework": FRAMEWORK_SGLANG,
                "producer": "python_probe",
                "domain": "cache_io",
                "event_kind": "movement",
                "python_module": getattr(original, "__module__", "sgl_kernel.kvcacheio"),
                "python_function": function_name,
                "transfer_scope": "kernel_kvcacheio",
                "direction": direction,
                "transfer_direction": _direction_text(transfer_direction),
                "layout": _layout_from_name(function_name),
                "num_tokens": count,
                "num_pages": num_pages,
                "page_size": page_size,
                "tier_src": tier_src,
                "tier_dst": tier_dst,
            }
            writer = get_writer()
            start = writer.now_us()
            try:
                result = original(*args, **kwargs)
                event_args["status"] = "ok"
                return result
            except Exception as exc:
                event_args["status"] = "exception"
                event_args["exception"] = type(exc).__name__
                raise
            finally:
                writer.duration_event(HICACHE_TRANSFER_KV_DIM_EXCHANGE, start, writer.now_us(), HICACHE_CATEGORY, event_args)

        return wrapped

    return make


def install(module) -> bool:
    changed = False
    for function_name in TRANSFER_FUNCTIONS:
        changed = wrap_function(module, function_name, _wrapper(function_name)) or changed
    return changed
