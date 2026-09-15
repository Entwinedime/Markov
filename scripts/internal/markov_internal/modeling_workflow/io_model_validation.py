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
    points = [
        {name: check(raw[name], f"{context}.{name}") for name, check in fields.items()}
        for raw in value
    ]
    keys = [tuple(point[name] for name in coordinates) for point in points]
    if keys != sorted(set(keys)):
        raise ValueError(f"{context} coordinates must be increasing and unique")
    return points


def _exact_object(value: Any, fields: set[str], context: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != fields:
        raise ValueError(f"{context} must contain only {sorted(fields)}")
    return value


def required_service_models(value: Any) -> dict[str, dict[str, Any]]:
    raw_models = _exact_object(value, set(OPERATION_KINDS), "service_models")
    return {kind: _service_model(kind, raw_models[kind]) for kind in OPERATION_KINDS}


def _service_model(kind: str, value: Any) -> dict[str, Any]:
    context = f"service_models.{kind}"
    if not isinstance(value, dict) or value.get("direction") != KIND_DIRECTIONS[kind]:
        raise ValueError(f"{context} has an invalid direction")
    model: dict[str, Any] = {"direction": KIND_DIRECTIONS[kind]}
    runtime_fields = {"page_bytes": positive_u64, "runtime_scale": positive_finite_number}
    bandwidth_fields = {"page_bytes": positive_u64, "setup_us_per_operation": nonnegative_finite_number,
                        "bandwidth_bytes_per_sec": positive_finite_number}
    if kind == "prefetch":
        _exact_object(
            value,
            {"direction", "setup_us_per_operation", "setup_us_per_page", "bandwidth_bytes_per_sec",
             "runtime_scale_points"},
            context,
        )
        model.update(
            setup_us_per_operation=nonnegative_finite_number(value["setup_us_per_operation"], f"{context}.setup"),
            setup_us_per_page=nonnegative_finite_number(value["setup_us_per_page"], f"{context}.page_setup"),
            bandwidth_bytes_per_sec=positive_finite_number(value["bandwidth_bytes_per_sec"], f"{context}.bandwidth"),
            runtime_scale_points=_points(value["runtime_scale_points"], f"{context}.runtime_scale_points",
                                         runtime_fields, ("page_bytes",)),
        )
    elif kind in {"load", "write_device_to_host"}:
        _exact_object(value, {"direction", "page_bandwidth_points", "runtime_scale_points"}, context)
        model.update(
            page_bandwidth_points=_points(value["page_bandwidth_points"], f"{context}.page_bandwidth_points",
                                          bandwidth_fields, ("page_bytes",)),
            runtime_scale_points=_points(value["runtime_scale_points"], f"{context}.runtime_scale_points",
                                         runtime_fields, ("page_bytes",)),
        )
    else:
        _exact_object(value, {"direction", "new_operation_points", "existing_key_bandwidth_points",
                              "existing_runtime_scale_points"}, context)
        model.update(
            new_operation_points=_points(
                value["new_operation_points"], f"{context}.new_operation_points",
                {"page_bytes": positive_u64, "setup_us_per_operation": nonnegative_finite_number,
                 "bandwidth_bytes_per_sec": positive_finite_number}, ("page_bytes",),
            ),
            existing_key_bandwidth_points=_points(
                value["existing_key_bandwidth_points"], f"{context}.existing_key_bandwidth_points",
                {"page_bytes": positive_u64, "operation_pages": positive_u64,
                 "bandwidth_bytes_per_sec": positive_finite_number}, ("page_bytes", "operation_pages"),
            ),
            existing_runtime_scale_points=_points(
                value["existing_runtime_scale_points"], f"{context}.existing_runtime_scale_points",
                runtime_fields, ("page_bytes",),
            ),
        )
    return model


def required_control_models(value: Any) -> dict[str, dict[str, Any]]:
    raw_models = _exact_object(value, set(OPERATION_KINDS), "control_models")
    output = {}
    for kind in OPERATION_KINDS:
        names = ("fixed_us_per_operation", "state_check_us_per_operation") if kind == "prefetch" else (
            "fixed_us_per_operation",
        )
        raw = _exact_object(raw_models[kind], set(names), f"control_models.{kind}")
        output[kind] = {
            name: nonnegative_finite_number(raw[name], f"control_models.{kind}.{name}") for name in names
        }
    return output


def required_resource_lanes(value: Any) -> dict[str, str]:
    raw = _exact_object(value, {"storage_read", "storage_write"}, "resource_lanes")
    if any(raw[name] not in {"shared", "scope"} for name in raw):
        raise ValueError("storage resource lanes must be 'shared' or 'scope'")
    return {name: raw[name] for name in ("storage_read", "storage_write")}
