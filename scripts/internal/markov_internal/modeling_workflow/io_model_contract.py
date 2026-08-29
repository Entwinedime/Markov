"""Canonical identities and scalar validation for the HiCache I/O model."""

from __future__ import annotations

import math
from typing import Any


MAX_U64 = (1 << 64) - 1
OPERATION_KINDS = (
    "prefetch",
    "load",
    "write_device_to_host",
    "write_host_to_storage",
)
KIND_DIRECTIONS = {
    "prefetch": "storage_to_host",
    "load": "host_to_device",
    "write_device_to_host": "device_to_host",
    "write_host_to_storage": "host_to_storage",
}


def positive_u64(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0 or value > MAX_U64:
        raise ValueError(f"HiCache I/O model field '{field}' must be a positive uint64 integer")
    return value


def nonnegative_finite_number(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"HiCache I/O model field '{field}' must be a finite non-negative number")
    normalized = float(value)
    if not math.isfinite(normalized) or normalized < 0.0:
        raise ValueError(f"HiCache I/O model field '{field}' must be a finite non-negative number")
    return normalized


def positive_finite_number(value: Any, field: str) -> float:
    normalized = nonnegative_finite_number(value, field)
    if normalized <= 0.0:
        raise ValueError(f"HiCache I/O model field '{field}' must be a finite positive number")
    return normalized


def rounded_positive_u64(value: Any, field: str) -> int:
    normalized = positive_finite_number(value, field)
    rounded = int(round(normalized))
    if rounded <= 0 or rounded > MAX_U64:
        raise ValueError(f"HiCache I/O model field '{field}' cannot be represented as uint64")
    return rounded


def is_missing_contract_value(value: Any) -> bool:
    if value is None or value == "" or value == {}:
        return True
    return isinstance(value, int) and not isinstance(value, bool) and value == 0
