"""Repeat checks for the one fixed calibration input."""

from __future__ import annotations

from typing import Any
from statistics import median


def _optional_int(value: Any) -> int | None:
    """Keep an absent, operation-inapplicable counter absent."""

    return None if value is None else int(value)


def calibration_work(capture: dict[str, Any]) -> dict[str, list[tuple[Any, ...]]]:
    """Canonical actual work; timestamps, costs and request IDs are not identity."""

    io = []
    for row in capture["source_io_observations"]["observations"]:
        if not row["service_observed"]:
            continue
        batches = tuple(
            (
                int(batch["page_count"]),
                _optional_int(batch.get("storage_existing_page_count")),
                _optional_int(batch.get("storage_new_page_count")),
            )
            for batch in row.get("storage_service_batches", [])
        )
        io.append(
            (
                row["kind"],
                int(row["page_size"]),
                int(row["completed_tokens"]),
                int(row["service_page_count"]),
                int(row.get("operation_count") or 1),
                batches,
            )
        )
    phase = []
    for row in capture["source_phase_observations"]["observations"]:
        phase.append(
            (
                int(row["logical_input"]),
                int(row["source_page_size"]),
                int(row["batch_size"]),
                int(row["prompt_token_count"]),
                int(row["prefill_token_count"]),
                int(row["decode_iteration_count"]),
            )
        )
    return {"io": sorted(io), "phase": sorted(phase)}


def repeat_work_report(captures: list[dict[str, Any]]) -> dict[str, Any]:
    """Reject changed work within an endpoint; endpoint geometry may differ."""

    if not captures:
        return {"status": "missing", "capture_count": 0, "comparisons": []}
    by_page: dict[int, list[dict[str, Any]]] = {}
    for capture in captures:
        pages = {int(row["page_size"]) for row in capture["source_io_observations"]["observations"]
                 if int(row.get("page_size") or 0) > 0}
        if len(pages) != 1:
            raise ValueError("one fixed calibration profile must use one page endpoint")
        by_page.setdefault(pages.pop(), []).append(capture)
    points = []
    for page, values in sorted(by_page.items()):
        baseline = calibration_work(values[0])
        comparisons = []
        for capture in values[1:]:
            work = calibration_work(capture)
            comparisons.append({
                "source_manifest": capture["source_manifest"],
                "io_equal": work["io"] == baseline["io"],
                "phase_equal": work["phase"] == baseline["phase"],
                "io_operation_count": len(work["io"]),
                "phase_observation_count": len(work["phase"]),
            })
        points.append({
            "page_size": page,
            "status": "consistent" if all(row["io_equal"] and row["phase_equal"] for row in comparisons)
                      else "work_changed",
            "capture_count": len(values),
            "baseline_manifest": values[0]["source_manifest"],
            "baseline_io_operation_count": len(baseline["io"]),
            "baseline_phase_observation_count": len(baseline["phase"]),
            "comparisons": comparisons,
        })
    equal = all(point["status"] == "consistent" for point in points)
    return {
        "status": "consistent" if equal else "work_changed",
        "capture_count": len(captures),
        "points": points,
    }


def repeat_duration_report(captures: list[dict[str, Any]]) -> dict[str, Any]:
    """Report fixed-input timing spread without turning it into a fitted correction."""

    by_page: dict[int, list[dict[str, Any]]] = {}
    for capture in captures:
        pages = {int(row["page_size"]) for row in capture["source_io_observations"]["observations"]
                 if int(row.get("page_size") or 0) > 0}
        if len(pages) == 1:
            by_page.setdefault(pages.pop(), []).append(capture)
    rows = []
    for page, values in sorted(by_page.items()):
        totals: dict[str, list[float]] = {}
        for capture in values:
            per_kind: dict[str, float] = {}
            for observation in capture["source_io_observations"]["observations"]:
                if observation.get("service_observed"):
                    kind = str(observation.get("kind") or "")
                    per_kind[kind] = per_kind.get(kind, 0.0) + float(observation.get("service_us") or 0.0)
            for kind, duration in per_kind.items():
                totals.setdefault(kind, []).append(duration)
        for kind, durations in sorted(totals.items()):
            center = median(durations)
            relative_range = (max(durations) - min(durations)) / center if center > 0 and len(durations) > 1 else 0.0
            rows.append({
                "page_size": page,
                "kind": kind,
                "repeat_count": len(durations),
                "median_service_us": center,
                "relative_range": relative_range,
                "high_variability": relative_range > 0.10,
            })
    return {
        "status": "reported" if rows else "missing",
        "diagnostic_relative_range_threshold": 0.10,
        "high_variability_count": sum(row["high_variability"] for row in rows),
        "rows": rows,
    }
