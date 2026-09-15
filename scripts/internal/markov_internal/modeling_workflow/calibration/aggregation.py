"""Select physical samples and build the compact service calibration."""

from __future__ import annotations

import math
from pathlib import Path
from typing import Any

from .host_storage import STORAGE_SOURCE_WORKING_SET_RUNTIME
from .runtime_anchors import load_runtime_anchor_projection
from ..physical_calibration import percentile


def _is_sustained_new_write_point(row: dict[str, Any]) -> bool:
    return row.get("direction") == "host_to_storage" and row.get("resource_state") == "sustained"


def select_point_durations(samples: list[dict[str, Any]], bandwidth_percentile: float) -> list[dict[str, Any]]:
    """Select one service/resource duration for each physical grid coordinate."""

    fields = (
        "group",
        "direction",
        "bytes",
        "page_bytes",
        "page_count",
        "scope_count",
        "resource_state",
        "operation_pages_per_scope",
        "operation_bytes_per_scope",
        "operation_count",
        "batch_semantics",
        "source_working_set_semantics",
        "page_materialization",
    )
    grouped: dict[tuple[Any, ...], list[dict[str, Any]]] = {}
    for row in samples:
        grouped.setdefault(tuple(row.get(field) for field in fields), []).append(row)
    selected: list[dict[str, Any]] = []
    duration_percentile = 1.0 - bandwidth_percentile
    for rows in grouped.values():
        exemplar = rows[0]
        duration_ns = percentile([int(row["duration_ns"]) for row in rows], duration_percentile)
        service_ns = percentile([int(row["service_duration_ns"]) for row in rows], duration_percentile)
        selected.append(
            {field: exemplar[field] for field in fields if exemplar.get(field) is not None}
            | {
                "sample_count": len(rows),
                "selected_duration_ns": duration_ns,
                "selected_service_duration_ns": service_ns,
                "selected_service_bandwidth_bytes_per_sec": (
                    int(exemplar["bytes"]) * 1_000_000_000 // service_ns
                ),
            }
        )
    return sorted(
        selected,
        key=lambda row: (
            str(row.get("direction")),
            str(row.get("resource_state")),
            int(row.get("page_bytes") or 0),
            int(row.get("operation_bytes_per_scope") or 0),
        ),
    )


def build_final_capture_service_models(report: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """Build the four target-independent service primitives consumed by one-base fitting."""

    parameters = report.get("parameters")
    storage = report.get("host_storage")
    runtime = report.get("runtime_dma")
    if not isinstance(parameters, dict) or not isinstance(storage, dict) or not isinstance(runtime, dict):
        raise ValueError("calibration report is missing parameters or physical samples")
    page_bytes = [int(value) for value in parameters.get("page_bytes") or []]
    scope_count = int(parameters.get("tensor_parallel_size") or 0)
    services = _storage_service_models(list(storage.get("selected_points") or []))
    dma = load_runtime_anchor_projection(
        Path(str(runtime.get("concurrent_report") or "")),
        expected_kv_bytes_per_token_per_rank=int(
            (report.get("kv_geometry") or {}).get("kv_bytes_per_token_per_rank") or 0
        ),
        expected_page_bytes=page_bytes,
        expected_concurrent_scope_count=scope_count,
    )
    services.update(dma.service_models)
    return services


def _storage_service_models(points: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    warm_reads = [
        row
        for row in points
        if row.get("direction") == "storage_to_host" and row.get("resource_state") == "isolated_warm"
    ]
    existing_writes = [
        row
        for row in points
        if row.get("direction") == "host_to_storage" and row.get("resource_state") == "existing_key"
    ]
    new_writes = [
        row
        for row in points
        if _is_sustained_new_write_point(row)
        and row.get("batch_semantics") == "runtime_materialize_then_batch_set"
        and row.get("source_working_set_semantics") == STORAGE_SOURCE_WORKING_SET_RUNTIME
    ]
    if not warm_reads or not existing_writes or not new_writes:
        raise ValueError("storage calibration requires warm-read, existing-key, and runtime new-write points")
    page_setup, bandwidth = _fit_pages_bytes(warm_reads)
    return {
        "prefetch": {
            "direction": "storage_to_host",
            "setup_us_per_operation": 0.0,
            "setup_us_per_page": page_setup,
            "bandwidth_bytes_per_sec": bandwidth,
        },
        "write_host_to_storage": {
            "direction": "host_to_storage",
            "new_operation_points": _new_operation_points(new_writes),
            "existing_key_bandwidth_points": _existing_key_points(existing_writes),
        },
    }


def _existing_key_points(points: list[dict[str, Any]]) -> list[dict[str, Any]]:
    output = [
        {
            "page_bytes": int(point["page_bytes"]),
            "operation_pages": int(point["operation_pages_per_scope"]),
            "bandwidth_bytes_per_sec": float(point["selected_service_bandwidth_bytes_per_sec"]),
        }
        for point in sorted(
            points,
            key=lambda row: (int(row["page_bytes"]), int(row["operation_pages_per_scope"])),
        )
    ]
    coordinates = {(row["page_bytes"], row["operation_pages"]) for row in output}
    if len(coordinates) != len(output):
        raise ValueError("existing-key calibration contains duplicate coordinates")
    return output


def _new_operation_points(points: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_page: dict[int, list[dict[str, Any]]] = {}
    for point in points:
        by_page.setdefault(int(point["page_bytes"]), []).append(point)
    output: list[dict[str, Any]] = []
    for page_bytes, page_points in sorted(by_page.items()):
        if len({int(row["operation_bytes_per_scope"]) for row in page_points}) < 2:
            raise ValueError(f"new-write page size {page_bytes} requires at least two operation payloads")
        setup, us_per_byte = _nonnegative_lstsq(
            [
                (
                    float(row["operation_count"]),
                    float(row["bytes"]),
                    _selected_us(row),
                )
                for row in page_points
            ]
        )
        output.append(
            {
                "page_bytes": page_bytes,
                "setup_us_per_operation": setup,
                "bandwidth_bytes_per_sec": _rate(us_per_byte),
            }
        )
    return output


def _fit_pages_bytes(points: list[dict[str, Any]]) -> tuple[float, float]:
    if len({row["page_bytes"] for row in points}) == 1:
        # Bytes = pages * page_bytes: only the combined response is identifiable.
        # Represent it as an effective rate on this page domain, not two fitted costs.
        (us_per_byte,) = _nonnegative_lstsq([(float(row["bytes"]), _selected_us(row)) for row in points])
        return 0.0, _rate(us_per_byte)
    setup, us_per_byte = _nonnegative_lstsq(
        [
            (float(row["page_count"]), float(row["bytes"]), _selected_us(row))
            for row in points
        ]
    )
    if us_per_byte <= 0.0:
        raise ValueError("storage calibration did not identify a positive byte coefficient")
    return setup, _rate(us_per_byte)


def _selected_us(point: dict[str, Any]) -> float:
    """Total rank work pairs with summed rank service, not concurrent wall time."""
    return float(int(point["selected_service_duration_ns"])) / 1000.0


def _nonnegative_lstsq(rows: list[tuple[float, ...]]) -> list[float]:
    if not rows:
        raise ValueError("calibration fit requires samples")
    feature_count = len(rows[0]) - 1
    if feature_count not in (1, 2) or any(len(row) != feature_count + 1 for row in rows):
        raise ValueError("calibration fit supports one or two coefficients")
    if any(not all(math.isfinite(float(value)) for value in row) for row in rows):
        raise ValueError("calibration fit requires finite samples")

    # The physical model has at most two non-negative coefficients.  Enumerate
    # the zero, one-column and two-column fits instead of adding a numerical
    # package to the runtime image for this tiny problem.
    candidates = [[0.0] * feature_count]
    for index in range(feature_count):
        denominator = math.fsum(float(row[index]) ** 2 for row in rows)
        if denominator > 0.0:
            value = math.fsum(float(row[index]) * float(row[-1]) for row in rows) / denominator
            if value >= 0.0 and math.isfinite(value):
                candidate = [0.0] * feature_count
                candidate[index] = value
                candidates.append(candidate)
    if feature_count == 2:
        scales = [max(abs(float(row[index])) for row in rows) for index in range(2)]
        if all(scale > 0.0 for scale in scales):
            z0 = [float(row[0]) / scales[0] for row in rows]
            z1 = [float(row[1]) / scales[1] for row in rows]
            y = [float(row[-1]) for row in rows]
            a00 = math.fsum(value * value for value in z0)
            a01 = math.fsum(left * right for left, right in zip(z0, z1))
            a11 = math.fsum(value * value for value in z1)
            b0 = math.fsum(value * target for value, target in zip(z0, y))
            b1 = math.fsum(value * target for value, target in zip(z1, y))
            determinant = a00 * a11 - a01 * a01
            if determinant > 1e-12 * max(a00 * a11, 1.0):
                scaled = [(b0 * a11 - b1 * a01) / determinant,
                          (a00 * b1 - a01 * b0) / determinant]
                candidate = [scaled[index] / scales[index] for index in range(2)]
                if all(value >= 0.0 and math.isfinite(value) for value in candidate):
                    candidates.append(candidate)

    def residual(coefficients: list[float]) -> float:
        return math.fsum(
            (math.fsum(float(row[index]) * coefficients[index] for index in range(feature_count))
             - float(row[-1])) ** 2
            for row in rows
        )

    return min(candidates, key=residual)


def _rate(us_per_byte: float) -> float:
    if not math.isfinite(us_per_byte) or us_per_byte <= 0.0:
        raise ValueError("calibration did not identify a positive bandwidth")
    return 1_000_000.0 / us_per_byte
