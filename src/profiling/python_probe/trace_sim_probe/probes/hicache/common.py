"""HiCache probe 通用取值与安全转换 helper。"""

from __future__ import annotations

import os
from typing import Any

from trace_sim_probe.probes import generic_callable as _base


def _extract_source_value(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """委托通用 probe source 语法读取原始值。"""

    return _base._extract_raw_value(source, field_name, bound, args, kwargs, result)


def _scope_from_optional_source(
    source: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> str:
    """从可选 source 读取 cache_scope，缺失时返回空字符串。"""

    found, value = _extract_source_value(source, "cache_scope", bound, args, kwargs, result)
    return _cache_scope_key(value) if found else ""


def _cache_scope_key(value: Any) -> str:
    """生成包含 rank 和对象身份的 cache scope 路由键。"""

    rank = os.environ.get("RANK", os.environ.get("LOCAL_RANK", "unknown"))
    if value is None:
        return f"rank:{rank}:unknown"
    if isinstance(value, (str, int, float, bool)):
        return f"rank:{rank}:{value}"
    return f"rank:{rank}:{type(value).__name__}:{id(value)}"


def _safe_int(value: Any) -> int | None:
    """宽松解析整数，避免 None/bool 污染容量和长度字段。"""

    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _safe_call_int(obj: Any, method_name: str) -> int | None:
    """安全调用无参方法并解析整数结果。"""

    method = getattr(obj, method_name, None) if obj is not None else None
    if not callable(method):
        return None
    try:
        return _safe_int(method())
    except Exception:
        return None


def _page_count_from_tokens(tokens: int | None, page_size: int | None) -> int | None:
    """把 token 容量向下投影为完整 page 数。"""

    if tokens is None or page_size is None or page_size <= 0:
        return None
    return tokens // page_size


def _first_attr(obj: Any, names: tuple[str, ...]) -> Any:
    """按候选字段名顺序读取第一个存在的属性。"""

    if obj is None:
        return None
    for name in names:
        if hasattr(obj, name):
            return getattr(obj, name)
    return None


def _jsonable_compact(value: Any) -> Any:
    """把对象收敛为短 JSON 值，控制 trace payload 大小。"""

    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, (list, tuple)):
        return [_jsonable_compact(item) for item in value[:64]]
    if isinstance(value, dict):
        return {str(key): _jsonable_compact(item) for key, item in list(value.items())[:64]}
    return str(value)
