"""One Python boundary check for the compact HiCache model contract."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from .io_model_contract import (
    KIND_DIRECTIONS,
    OPERATION_KINDS,
    nonnegative_finite_number,
    positive_finite_number,
    positive_u64,
)


ScalarCheck = Callable[[Any, str], Any]


def _points(
    value: Any,
    context: str,
    fields: dict[str, ScalarCheck],
    coordinates: tuple[str, ...],
) -> list[dict[str, Any]]:
    """Validate one measured point table and its increasing coordinates."""

    if not isinstance(value, list) or not value:
        raise ValueError(f"{context} requires measured anchors")
    expected = set(fields)
    if any(not isinstance(raw, dict) or set(raw) != expected for raw in value):
        raise ValueError(f"{context} anchors must contain only {sorted(expected)}")
    points = [{name: check(raw[name], f"{context}.{name}") for name, check in fields.items()} for raw in value]
    keys = [tuple(point[name] for name in coordinates) for point in points]
    if keys != sorted(set(keys)):
        raise ValueError(f"{context} coordinates must be increasing and unique")
    return points


def _exact_object(value: Any, fields: set[str], context: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != fields:
        raise ValueError(f"{context} must contain only {sorted(fields)}")
    return value


def required_service_models(value: Any) -> dict[str, dict[str, Any]]:
    if not isinstance(value, dict) or set(value) - set(OPERATION_KINDS):
        raise ValueError("service_models must contain only supported service families")
    return {kind: _service_model(kind, raw) for kind, raw in value.items()}


def required_prefetch_stages(value: Any) -> dict[str, float]:
    context = "service_models.prefetch.stages"
    names = (
        "before_copy_us_per_page",
        "before_copy_us_per_byte",
        "copy_publish_us_per_page",
        "copy_publish_us_per_byte",
        "return_us_per_operation",
        "return_us_per_page",
    )
    stages = _exact_object(value, set(names), context)
    return {name: nonnegative_finite_number(stages[name], f"{context}.{name}") for name in names}


def _service_model(kind: str, value: Any) -> dict[str, Any]:
    context = f"service_models.{kind}"
    if not isinstance(value, dict) or value.get("direction") != KIND_DIRECTIONS[kind]:
        raise ValueError(f"{context} has an invalid direction")
    model: dict[str, Any] = {"direction": KIND_DIRECTIONS[kind]}
    runtime_fields = {"page_bytes": positive_u64, "runtime_scale": positive_finite_number}
    bandwidth_fields = {
        "page_bytes": positive_u64,
        "setup_us_per_operation": nonnegative_finite_number,
        "bandwidth_bytes_per_sec": positive_finite_number,
    }
    if kind == "prefetch":
        tables = {"runtime_scale_points": runtime_fields}
    elif kind in {"load", "write_device_to_host"}:
        tables = {"page_bandwidth_points": bandwidth_fields, "runtime_scale_points": runtime_fields}
    else:
        tables = {}
        if "new_operation_points" in value:
            tables["new_operation_points"] = bandwidth_fields
        if "existing_key_bandwidth_points" in value:
            tables["existing_key_bandwidth_points"] = {
                "page_bytes": positive_u64,
                "operation_pages": positive_u64,
                "bandwidth_bytes_per_sec": positive_finite_number,
            }
            tables["existing_runtime_scale_points"] = runtime_fields
        if not tables:
            raise ValueError(f"{context} requires new or existing write costs")

    _exact_object(value, {"direction", *tables} | ({"stages"} if kind == "prefetch" else set()), context)
    if kind == "prefetch":
        model["stages"] = required_prefetch_stages(value["stages"])

    for name, fields in tables.items():
        coordinates = ("page_bytes", "operation_pages") if "operation_pages" in fields else ("page_bytes",)
        model[name] = _points(value[name], f"{context}.{name}", fields, coordinates)

    return model


def required_resource_lanes(value: Any) -> dict[str, str]:
    raw = _exact_object(value, {"storage_read", "storage_write"}, "resource_lanes")
    if any(raw[name] not in {"shared", "scope"} for name in raw):
        raise ValueError("storage resource lanes must be 'shared' or 'scope'")
    return {name: raw[name] for name in ("storage_read", "storage_write")}
