"""Discover real workload timing windows used by modeling validation."""

from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.paths import require_repo_path


@dataclass(frozen=True)
class WorkloadWindow:
    """Observed trace-clock interval in nanoseconds, never a duration-only report."""

    report_path: Path
    start_ns: int
    end_ns: int
    actual_e2e_ns: int
    source: str = "workload_report"


def controlled_request_window(formal: WorkloadWindow) -> WorkloadWindow:
    """Use all controlled requests for independent cost measurement, not E2E scoring."""

    report = load_json(formal.report_path)
    requests = [row for row in report.get("requests", []) if isinstance(row, dict) and row.get("kind") == "request"]
    starts = [float(row["start_time_ms"]) for row in requests if row.get("start_time_ms") is not None]
    ends = [float(row["end_time_ms"]) for row in requests if row.get("end_time_ms") is not None]
    if len(starts) != len(requests) or len(ends) != len(requests) or not requests:
        raise ValueError("fixed calibration requires timestamps for every controlled request")
    start_ns = int(min(starts) * 1_000_000)
    end_ns = int(max(ends) * 1_000_000)
    return WorkloadWindow(formal.report_path, start_ns, end_ns, end_ns - start_ns, "all_controlled_requests")


def discover_workload_window(input_cfg: dict[str, Any], manifest_path: Path | None) -> WorkloadWindow | None:
    """Discover a workload interval from explicit config or profile artifacts."""

    explicit = input_cfg.get("workload_report")
    if isinstance(explicit, str):
        path = require_repo_path(explicit)
        if path.suffix == ".jsonl":
            raise ValueError(
                "bench-serving duration alone cannot define a trace window; provide a timestamped workload report"
            )
        return load_workload_window(path)
    if manifest_path is None:
        return None
    manifest = load_json(manifest_path)
    reports = manifest.get("bench", {}).get("workload_report_files", [])
    if len(reports) > 1:
        raise ValueError("multiple workload reports; select input.workload_report explicitly")
    if reports:
        return load_workload_window(require_repo_path(reports[0]["path"]))
    # bench_serving_files contain aggregate durations, not trace-clock bounds.
    # Leave generic DAG builds unfiltered; window-dependent workflows reject None.
    return None


def load_workload_window(path: Path) -> WorkloadWindow | None:
    """Derive the request envelope from a ``workload_report.json`` file."""

    report = load_json(path)
    formal_window = report.get("formal_window")
    if formal_window is not None and not isinstance(formal_window, dict):
        raise ValueError(f"invalid formal workload window in workload report: {path}")
    formal_source = formal_window if isinstance(formal_window, dict) else report
    formal_start_ms = optional_float(formal_source.get("formal_begin_ms"))
    formal_end_ms = optional_float(formal_source.get("formal_end_ms"))
    if formal_window is not None or formal_start_ms is not None or formal_end_ms is not None:
        if (
            formal_start_ms is None
            or formal_end_ms is None
            or not math.isfinite(formal_start_ms)
            or not math.isfinite(formal_end_ms)
            or formal_end_ms <= formal_start_ms
        ):
            raise ValueError(f"invalid formal workload window in workload report: {path}")
        start_ns = int(formal_start_ms * 1_000_000)
        end_ns = int(formal_end_ms * 1_000_000)
        actual_e2e_ns = end_ns - start_ns
        formal_e2e_raw = formal_source.get("e2e_ms")
        if formal_e2e_raw is not None:
            formal_e2e_ms = optional_float(formal_e2e_raw)
            if formal_e2e_ms is None or not math.isfinite(formal_e2e_ms) or formal_e2e_ms <= 0:
                raise ValueError(f"invalid formal workload E2E in workload report: {path}")
            actual_e2e_ns = int(formal_e2e_ms * 1_000_000)
            if abs(actual_e2e_ns - (end_ns - start_ns)) > 1_000_000:
                raise ValueError(f"formal workload E2E does not match its window: {path}")
        return WorkloadWindow(
            path,
            start_ns,
            end_ns,
            actual_e2e_ns,
            "workload_report.formal_window",
        )
    requests = report.get("requests")
    if not isinstance(requests, list):
        return None
    starts: list[int] = []
    ends: list[int] = []
    for row in requests:
        if not isinstance(row, dict):
            continue
        start = optional_float(row.get("start_time_ms"))
        end = optional_float(row.get("end_time_ms"))
        if start is None or end is None:
            continue
        starts.append(int(start * 1_000_000))
        ends.append(int(end * 1_000_000))
    if not starts or not ends:
        return None
    return WorkloadWindow(path, min(starts), max(ends), max(ends) - min(starts), "workload_report")


def optional_float(value: Any) -> float | None:
    """Parse a float candidate, returning ``None`` on conversion failure."""

    try:
        return float(value)
    except (TypeError, ValueError):
        return None
