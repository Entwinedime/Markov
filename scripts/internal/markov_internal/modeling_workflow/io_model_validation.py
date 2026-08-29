"""Boundary validation for the compact, interpretable HiCache I/O model."""

from __future__ import annotations

from typing import Any

from .io_model_contract import (
    KIND_DIRECTIONS,
    OPERATION_KINDS,
    nonnegative_finite_number,
    positive_finite_number,
    positive_u64,
)


def required_service_models(value: Any) -> dict[str, dict[str, Any]]:
    if not isinstance(value, dict) or set(value) != set(OPERATION_KINDS):
        raise ValueError("service_models must contain the four HiCache I/O families")
    return {kind: _service_model(kind, value[kind]) for kind in OPERATION_KINDS}


def _service_model(kind: str, raw: Any) -> dict[str, Any]:
    if not isinstance(raw, dict) or raw.get("direction") != KIND_DIRECTIONS[kind]:
        raise ValueError(f"service_models.{kind} has an invalid direction")
    model: dict[str, Any] = {"direction": KIND_DIRECTIONS[kind]}
    if kind == "prefetch":
        model.update(
            setup_us_per_operation=_nonnegative(raw, "setup_us_per_operation", kind),
            setup_us_per_page=_nonnegative(raw, "setup_us_per_page", kind),
            bandwidth_bytes_per_sec=_positive(raw, "bandwidth_bytes_per_sec", kind),
            runtime_scale=_positive(raw, "runtime_scale", kind),
        )
    elif kind in {"load", "write_device_to_host"}:
        model.update(
            page_bandwidth_points=_page_bandwidth_points(raw.get("page_bandwidth_points"), kind),
            runtime_scale=_positive(raw, "runtime_scale", kind),
        )
    else:
        model.update(
            new_operation_points=_new_operation_points(raw.get("new_operation_points")),
            existing_key_bandwidth_points=_existing_key_points(raw.get("existing_key_bandwidth_points")),
            existing_runtime_scale=_positive(raw, "existing_runtime_scale", kind),
            new_runtime_scale=_positive(raw, "new_runtime_scale", kind),
        )
    return model


def required_control_models(value: Any) -> dict[str, dict[str, float]]:
    if not isinstance(value, dict) or set(value) != set(OPERATION_KINDS):
        raise ValueError("control_models must contain the four HiCache I/O families")
    output: dict[str, dict[str, float]] = {}
    for kind in OPERATION_KINDS:
        raw = value[kind]
        if not isinstance(raw, dict):
            raise ValueError(f"control_models.{kind} must be an object")
        output[kind] = {
            "fixed_us_per_operation": nonnegative_finite_number(
                raw.get("fixed_us_per_operation", 0.0), f"control_models.{kind}.fixed_us_per_operation"
            ),
            "zero_payload_fixed_us_per_operation": nonnegative_finite_number(
                raw.get("zero_payload_fixed_us_per_operation", 0.0),
                f"control_models.{kind}.zero_payload_fixed_us_per_operation",
            ),
            "per_page_us": nonnegative_finite_number(
                raw.get("per_page_us", 0.0), f"control_models.{kind}.per_page_us"
            ),
        }
    return output


def required_resource_lanes(value: Any) -> dict[str, str]:
    if not isinstance(value, dict):
        raise ValueError("resource_lanes must be an object")
    read = value.get("storage_read")
    write = value.get("storage_write")
    if read not in {"shared", "scope"} or write not in {"shared", "scope"}:
        raise ValueError("storage resource lanes must be 'shared' or 'scope'")
    return {"storage_read": read, "storage_write": write}


def required_planning_rates(value: Any) -> dict[str, int]:
    if not isinstance(value, dict):
        raise ValueError("planning_rates must be an object")
    return {
        "device_host_bytes_per_sec": positive_u64(
            value.get("device_host_bytes_per_sec"), "planning_rates.device_host_bytes_per_sec"
        ),
        "host_storage_bytes_per_sec": positive_u64(
            value.get("host_storage_bytes_per_sec"), "planning_rates.host_storage_bytes_per_sec"
        ),
    }


def _nonnegative(raw: dict[str, Any], field: str, kind: str) -> float:
    return nonnegative_finite_number(raw.get(field, 0.0), f"service_models.{kind}.{field}")


def _positive(raw: dict[str, Any], field: str, kind: str) -> float:
    return positive_finite_number(raw.get(field), f"service_models.{kind}.{field}")


def _page_bandwidth_points(value: Any, kind: str) -> list[dict[str, Any]]:
    if not isinstance(value, list) or len(value) < 2:
        raise ValueError(f"service_models.{kind}.page_bandwidth_points requires at least two anchors")
    points = [
        {
            "page_bytes": positive_u64(raw.get("page_bytes"), f"service_models.{kind}.page_bytes"),
            "bandwidth_bytes_per_sec": positive_finite_number(
                raw.get("bandwidth_bytes_per_sec"), f"service_models.{kind}.bandwidth_bytes_per_sec"
            ),
        }
        for raw in value
        if isinstance(raw, dict)
    ]
    pages = [point["page_bytes"] for point in points]
    if len(points) != len(value) or pages != sorted(set(pages)):
        raise ValueError(f"service_models.{kind}.page_bandwidth_points must have increasing unique page anchors")
    return points


def _new_operation_points(value: Any) -> list[dict[str, Any]]:
    if not isinstance(value, list) or len(value) < 2:
        raise ValueError("H2S new_operation_points requires at least two anchors")
    points = [
        {
            "page_bytes": positive_u64(raw.get("page_bytes"), "H2S new page_bytes"),
            "setup_us_per_operation": nonnegative_finite_number(
                raw.get("setup_us_per_operation"), "H2S new setup_us_per_operation"
            ),
            "bandwidth_bytes_per_sec": positive_finite_number(
                raw.get("bandwidth_bytes_per_sec"), "H2S new bandwidth_bytes_per_sec"
            ),
        }
        for raw in value
        if isinstance(raw, dict)
    ]
    _increasing_pages(points, len(value), "H2S new_operation_points")
    return points


def _existing_key_points(value: Any) -> list[dict[str, Any]]:
    if not isinstance(value, list) or len(value) < 4:
        raise ValueError("H2S existing_key_bandwidth_points requires a calibrated page/payload curve")
    points = [
        {
            "page_bytes": positive_u64(raw.get("page_bytes"), "H2S existing page_bytes"),
            "operation_pages": positive_u64(raw.get("operation_pages"), "H2S existing operation_pages"),
            "bandwidth_bytes_per_sec": positive_finite_number(
                raw.get("bandwidth_bytes_per_sec"), "H2S existing bandwidth_bytes_per_sec"
            ),
        }
        for raw in value
        if isinstance(raw, dict)
    ]
    coordinates = [(point["page_bytes"], point["operation_pages"]) for point in points]
    if len(points) != len(value) or coordinates != sorted(set(coordinates)):
        raise ValueError("H2S existing_key_bandwidth_points must have increasing unique coordinates")
    return points


def _increasing_pages(points: list[dict[str, Any]], expected: int, field: str) -> None:
    pages = [point["page_bytes"] for point in points]
    if len(points) != expected or pages != sorted(set(pages)):
        raise ValueError(f"{field} must have increasing unique page anchors")
