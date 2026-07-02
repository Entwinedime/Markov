"""HiCache capacity oracle 的值解析辅助工具。"""

from __future__ import annotations

from typing import Any


def unique_int_values(unique_values: dict[str, Any], keys: list[str]) -> list[int]:
    """从 capacity oracle unique_values 中收集整数值。"""

    values: set[int] = set()
    for key in keys:
        raw_values = unique_values.get(key)
        if not isinstance(raw_values, list):
            continue
        for value in raw_values:
            item = parse_int_or_none(value)
            if item is not None:
                values.add(item)
    return sorted(values)


def unique_policy_values(unique_values: dict[str, Any], keys: list[str]) -> list[str]:
    """从 capacity oracle unique_values 中收集规整后的 policy 值。"""

    values: set[str] = set()
    for key in keys:
        raw_values = unique_values.get(key)
        if not isinstance(raw_values, list):
            continue
        for value in raw_values:
            normalized = normalize_policy_value(value)
            if normalized:
                values.add(normalized)
    return sorted(values)


def parse_int_or_none(value: Any) -> int | None:
    """严格解析整数候选；bool 和非法值返回 None。"""

    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def normalize_policy_value(value: Any) -> str:
    """规整 policy 字符串，使用下划线形式。"""

    if value is None:
        return ""
    return str(value).strip().lower().replace("-", "_")


def flatten_hicache_capacity_scalars(value: Any, prefix: str = "") -> list[tuple[str, Any]]:
    """把 capacity snapshot 中的嵌套标量展开成点分路径。"""

    rows: list[tuple[str, Any]] = []
    if isinstance(value, dict):
        for key, item in sorted(value.items()):
            child_prefix = f"{prefix}.{key}" if prefix else str(key)
            rows.extend(flatten_hicache_capacity_scalars(item, child_prefix))
        return rows
    if value is None or isinstance(value, (list, tuple, set)):
        return rows
    if isinstance(value, (str, int, float, bool)):
        rows.append((prefix, value))
    return rows
