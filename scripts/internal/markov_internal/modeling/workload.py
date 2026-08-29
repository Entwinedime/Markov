"""Discover real workload timing windows used by modeling validation."""

from __future__ import annotations

import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.paths import map_repo_path, require_repo_path


@dataclass(frozen=True)
class WorkloadWindow:
    """Observed workload interval represented in nanoseconds."""

    report_path: Path
    start_ns: int
    end_ns: int
    actual_e2e_ns: int
    source: str = "workload_report"


def discover_workload_window(input_cfg: dict[str, Any], manifest_path: Path | None) -> WorkloadWindow | None:
    """Discover a workload interval from explicit config or profile artifacts."""

    explicit = input_cfg.get("workload_report")
    if isinstance(explicit, str):
        return load_workload_window(require_repo_path(explicit))
    if manifest_path is None or not manifest_path.is_file():
        return None
    manifest = load_json(manifest_path)
    run_dir_raw = manifest.get("run_dir")
    run_dir = map_repo_path(Path(str(run_dir_raw))) if isinstance(run_dir_raw, str) else manifest_path.parent
    candidates = sorted(run_dir.glob("bench/**/workload_report.json"))
    if candidates:
        return load_workload_window(candidates[-1])
    bench_candidates = sorted(run_dir.glob("bench/**/*.jsonl"))
    for path in reversed(bench_candidates):
        window = load_bench_serving_window(path)
        if window is not None:
            return window
    return None


def load_workload_window(path: Path) -> WorkloadWindow | None:
    """Derive the request envelope from a ``workload_report.json`` file."""

    if not path.is_file():
        return None
    report = load_json(path)
    formal_window = report.get("formal_window")
    if formal_window is not None and not isinstance(formal_window, dict):
        raise ValueError(f"invalid formal workload window in workload report: {path}")
    formal_source = formal_window if isinstance(formal_window, dict) else report
    formal_start_ms = optional_float(formal_source.get("formal_begin_ms"))
    formal_end_ms = optional_float(formal_source.get("formal_end_ms"))
    if formal_start_ms is not None or formal_end_ms is not None:
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


def load_bench_serving_window(path: Path) -> WorkloadWindow | None:
    """Read aggregate duration from the final SGLang bench-serving JSONL row."""

    if not path.is_file():
        return None
    last: dict[str, Any] | None = None
    with path.open("r", encoding="utf-8") as file_obj:
        for line in file_obj:
            line = line.strip()
            if not line:
                continue
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict):
                last = value
    if last is None:
        return None

    duration_s = optional_float(last.get("duration"))
    if duration_s is None or duration_s <= 0:
        return None
    actual = int(duration_s * 1_000_000_000)
    return WorkloadWindow(path, 0, actual, actual, "sglang_bench_serving_duration")


def optional_float(value: Any) -> float | None:
    """Parse a float candidate, returning ``None`` on conversion failure."""

    try:
        return float(value)
    except (TypeError, ValueError):
        return None
