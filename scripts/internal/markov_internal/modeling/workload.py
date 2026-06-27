"""Workload timing window discovery for modeling validation."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.paths import map_repo_path, resolve_repo_path


@dataclass(frozen=True)
class WorkloadWindow:
    """workload 真实耗时窗口。"""

    report_path: Path
    start_ns: int
    end_ns: int
    actual_e2e_ns: int
    source: str = "workload_report"


def discover_workload_window(input_cfg: dict[str, Any], manifest_path: Path | None) -> WorkloadWindow | None:
    """从显式配置或 profile manifest 中发现 workload 真实时间窗。"""

    explicit = input_cfg.get("workload_report")
    if isinstance(explicit, str):
        return load_workload_window(required_repo_path(explicit))
    if manifest_path is None or not manifest_path.is_file():
        return None
    manifest = load_json(manifest_path)
    run_dir_raw = manifest.get("run_dir")
    run_dir = map_repo_path(Path(str(run_dir_raw))) if isinstance(run_dir_raw, str) else manifest_path.parent
    candidates = sorted(run_dir.glob("bench/**/workload_report.json"))
    if candidates:
        return load_workload_window(candidates[-1])
    bench_candidates = sorted(path for path in run_dir.glob("bench/**/*.jsonl") if path.name != "workload_report.jsonl")
    for path in reversed(bench_candidates):
        window = load_bench_serving_window(path)
        if window is not None:
            return window
    return None


def load_workload_window(path: Path) -> WorkloadWindow | None:
    """从 workload_report.json 读取请求开始/结束时间窗。"""

    if not path.is_file():
        return None
    report = load_json(path)
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
    """从 SGLang bench serving JSONL 读取整体 duration。"""

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
    """宽松解析 float，失败时返回 None。"""

    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def required_repo_path(value: Any) -> Path:
    """Resolve a required repo path."""

    path = resolve_repo_path(value)
    if path is None:
        raise ValueError("expected a non-empty path")
    return path
