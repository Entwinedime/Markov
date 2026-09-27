"""Select physical samples and build the compact service calibration."""

from __future__ import annotations

import math
from collections import defaultdict
from statistics import median
from typing import Any

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
                "selected_service_bandwidth_bytes_per_sec": (int(exemplar["bytes"]) * 1_000_000_000 // service_ns),
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


def build_prefetch_service_model(samples: list[dict[str, Any]]) -> dict[str, Any]:
    """Pages/bytes explain physical work; batch cleanup follows buffer count.

    Only measured leaf calls and the measured final-publication-to-batch-return
    interval enter these fits. Observation glue is not assigned a free coefficient.
    Tail intervals still contain observation overhead; retain raw measurements.
    """
    before, copying, tails = defaultdict(list), defaultdict(list), defaultdict(list)
    for row in samples:
        events, page_bytes = row["events"], row["page_bytes"]
        for batch in (event for event in events if event["stage"] == "batch"):
            inside = [
                event
                for event in events
                if event["stage"] != "batch"
                and batch["start_ns"] <= event["start_ns"] <= event["end_ns"] <= batch["end_ns"]
            ]
            publications = [event for event in inside if event["stage"] == "publish"]
            copies = [event for event in inside if event["stage"] == "copy"]
            pages = batch["page_count"]
            if not publications or len(publications) != len(copies):
                raise ValueError("prefetch physical batch lacks copy/publication pairs")
            prefix_ns = sum(
                event["end_ns"] - event["start_ns"] for event in inside if event["stage"] in {"allocate", "read"}
            )
            copying_ns = sum(event["end_ns"] - event["start_ns"] for event in copies + publications)
            before[(page_bytes, pages)].append(prefix_ns / (1000 * pages))
            copying[(page_bytes, pages)].append(copying_ns / (1000 * len(copies)))
            # An injected cancellation immediately after a successful final
            # publication is test control, not the batch's cleanup work.
            end_publish = max(event["end_ns"] for event in publications)
            injected_ns = sum(
                event["end_ns"] - event["start_ns"]
                for event in inside
                if event["stage"] == "injected_cancel" and event["start_ns"] >= end_publish
            )
            tails[pages].append((batch["end_ns"] - end_publish - injected_ns) / 1000)
    prefix = _nonnegative_lstsq([(1.0, float(size), median(values)) for (size, _), values in sorted(before.items())])
    copy = _nonnegative_lstsq([(1.0, float(size), median(values)) for (size, _), values in sorted(copying.items())])
    tail = _nonnegative_lstsq([(1.0, float(pages), median(values)) for pages, values in sorted(tails.items())])
    stages = dict(
        zip(
            (
                "before_copy_us_per_page",
                "before_copy_us_per_byte",
                "copy_publish_us_per_page",
                "copy_publish_us_per_byte",
                "return_us_per_operation",
                "return_us_per_page",
            ),
            prefix + copy + tail,
        )
    )
    return {"direction": "storage_to_host", "stages": stages}


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
                scaled = [(b0 * a11 - b1 * a01) / determinant, (a00 * b1 - a01 * b0) / determinant]
                candidate = [scaled[index] / scales[index] for index in range(2)]
                if all(value >= 0.0 and math.isfinite(value) for value in candidate):
                    candidates.append(candidate)

    def residual(coefficients: list[float]) -> float:
        return math.fsum(
            (math.fsum(float(row[index]) * coefficients[index] for index in range(feature_count)) - float(row[-1])) ** 2
            for row in rows
        )

    return min(candidates, key=residual)


def _rate(us_per_byte: float) -> float:
    if not math.isfinite(us_per_byte) or us_per_byte <= 0.0:
        raise ValueError("calibration did not identify a positive bandwidth")
    return 1_000_000.0 / us_per_byte
