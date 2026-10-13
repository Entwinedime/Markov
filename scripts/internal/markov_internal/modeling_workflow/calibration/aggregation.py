"""Select measured physical responses; cost fitting belongs to offline models."""

from __future__ import annotations

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
