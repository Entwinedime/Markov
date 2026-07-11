"""Field-level checks for HiCache state-model facts."""

from __future__ import annotations

from typing import Any


def batch_state_fact_errors(args: dict[str, Any], role: str) -> list[str]:
    """Validate aligned arrays in a batch-level cache-extend fact."""

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
    """Validate request identity and positions for ``cache_extend_input``."""

    errors: list[str] = []
    string_ids = [str(item or "") for item in request_ids]
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
            if 0 <= index < expected and row_request_id and string_ids[index] and row_request_id != string_ids[index]:
                errors.append("request_positions.request_id")
        if sorted(indexes) != list(range(expected)):
            errors.append("request_positions.coverage")
    return errors


def has_fact(value: Any) -> bool:
    """Return whether a value is present and usable as a fact field."""

    if value is None or isinstance(value, bool):
        return False
    if isinstance(value, str):
        return bool(value)
    if isinstance(value, (list, tuple, set, dict)):
        return len(value) > 0
    return True


def has_token_dictionary(value: Any) -> bool:
    """Return whether a token dictionary contains model identity fields."""

    return (
        isinstance(value, dict)
        and isinstance(value.get("token_path_id"), str)
        and bool(value.get("token_path_id"))
        and has_fact(value.get("token_count"))
        and has_fact(value.get("hash_algo"))
    )


def has_token_span(value: Any) -> bool:
    """Return whether a token span can reference a recorded dictionary."""

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
    """Parse an integer permissively while rejecting booleans."""

    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None
