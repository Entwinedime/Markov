"""Canonical I/O identities and scalar admission for independent costs."""

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
DMA_SERVICES = {KIND_DIRECTIONS[kind]: kind for kind in ("load", "write_device_to_host")}
RUNTIME_DMA_OPERATOR_NAME = "npu::transfer_kv_dim_exchange"
RUNTIME_DMA_INDEX_RESIDENCY = {"host_indices": "cpu", "device_indices": "cpu"}
READY_IO_OBSERVATION_STATUSES = frozenset(
    {"ready", "ready_background_unmaterialized", "ready_background_transfer_only"}
)


class MissingCostEvidence(ValueError):
    """A supported cost form lacks measurements that identify its parameters."""


def io_observation_ready(observed: dict[str, Any]) -> bool:
    """Mirror the C++ ledger's usable operation states."""

    return observed.get("status") in READY_IO_OBSERVATION_STATUSES


def positive_u64(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0 or value > MAX_U64:
        raise ValueError(f"HiCache model field '{field}' must be a positive uint64 integer")
    return value


def nonnegative_finite_number(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"HiCache model field '{field}' must be a finite non-negative number")
    normalized = float(value)
    if not math.isfinite(normalized) or normalized < 0.0:
        raise ValueError(f"HiCache model field '{field}' must be a finite non-negative number")
    return normalized


def positive_finite_number(value: Any, field: str) -> float:
    normalized = nonnegative_finite_number(value, field)
    if normalized <= 0.0:
        raise ValueError(f"HiCache model field '{field}' must be a finite positive number")
    return normalized
