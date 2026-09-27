"""HiCache probe 通用取值与安全转换 helper。"""

from __future__ import annotations

import os
from typing import Any

from trace_sim_probe.probes.generic_callable import _extract_raw_value as _extract_source_value


def _scope_from_optional_source(
    source: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    result: Any,
) -> str:
    """从可选 source 读取 cache_scope，缺失时返回空字符串。"""

    found, value = _extract_source_value(source, "cache_scope", bound, args, result)
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
