"""HiCache state fact 字段级合同检查。"""

from __future__ import annotations

from typing import Any


def batch_state_fact_errors(args: dict[str, Any], role: str) -> list[str]:
    """检查 batch-level cache extend fact 的数组合同。"""

    if role != "cache_extend_input":
        return []
    errors: list[str] = []
    if str(args.get("batch_kind") or "") != "extend":
        errors.append("batch_kind.extend")
    request_ids = args.get("request_ids")
    request_positions = args.get("request_positions")
    token_dictionaries = args.get("token_dictionaries")
    full_path_spans = args.get("full_path_spans")
    token_counts = args.get("token_counts")
    if not isinstance(request_ids, list) or not request_ids:
        errors.append("request_ids.non_empty")
        request_ids = []
    expected = len(request_ids)
    for field_name, value in (
        ("request_positions", request_positions),
        ("token_dictionaries", token_dictionaries),
        ("full_path_spans", full_path_spans),
        ("token_counts", token_counts),
    ):
        if not isinstance(value, list):
            errors.append(f"{field_name}.array")
        elif len(value) != expected:
            errors.append(f"{field_name}.length")
    batch_size = int_or_none(args.get("batch_size"))
    if batch_size is None or batch_size != expected:
        errors.append("batch_size.request_ids_length")
    errors.extend(batch_position_errors(request_ids, request_positions, expected))
    return errors


def batch_position_errors(request_ids: list[Any], request_positions: Any, expected: int) -> list[str]:
    """检查 cache_extend_input 的 request_positions 合同。"""

    errors: list[str] = []
    string_ids = [str(item) for item in request_ids if item is not None]
    if len(string_ids) != expected or any(not item for item in string_ids):
        errors.append("request_ids.valid")
    if len(set(string_ids)) != len(string_ids):
        errors.append("request_ids.unique")
    if isinstance(request_positions, list) and len(request_positions) == expected:
        indexes: list[int] = []
        for row in request_positions:
            if not isinstance(row, dict):
                errors.append("request_positions.item")
                continue
            index = int_or_none(row.get("index"))
            if index is None:
                errors.append("request_positions.index")
                continue
            indexes.append(index)
            row_request_id = str(row.get("request_id") or "")
            if 0 <= index < expected and row_request_id and row_request_id != string_ids[index]:
                errors.append("request_positions.request_id")
        if sorted(indexes) != list(range(expected)):
            errors.append("request_positions.coverage")
    return errors


def fact_items(value: Any) -> list[Any]:
    """把 scalar field 和数组 field 统一成可遍历项。"""

    if isinstance(value, list):
        return value
    if value is None:
        return []
    return [value]


def has_fact(value: Any) -> bool:
    """判断字段值是否能作为有效事实参与合同检查。"""

    if value is None:
        return False
    if isinstance(value, str):
        return bool(value)
    if isinstance(value, (list, tuple, set, dict)):
        return len(value) > 0
    return True


def has_token_dictionary(value: Any) -> bool:
    """判断 token dictionary 是否包含模型所需的身份字段。"""

    return (
        isinstance(value, dict)
        and isinstance(value.get("token_path_id"), str)
        and bool(value.get("token_path_id"))
        and has_fact(value.get("token_count"))
        and has_fact(value.get("hash_algo"))
    )


def has_token_span(value: Any) -> bool:
    """判断 token span 是否能引用已记录 token dictionary。"""

    return (
        isinstance(value, dict)
        and isinstance(value.get("path_id"), str)
        and bool(value.get("path_id"))
        and has_fact(value.get("begin"))
        and has_fact(value.get("end"))
        and has_fact(value.get("token_count"))
        and has_fact(value.get("hash_algo"))
    )


def int_or_none(value: Any) -> int | None:
    """宽松解析整数，避免 bool 被误当成 0/1。"""

    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None
