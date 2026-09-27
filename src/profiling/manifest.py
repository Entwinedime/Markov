"""Profile manifest 生成。"""

from __future__ import annotations

from glob import escape
from pathlib import Path
from typing import Any

from .config import ProfilingRuntimeConfig
from .profiler_clock import read_host_clock


def build_profile_manifest(
    *,
    run_dir: Path,
    cfg: dict[str, Any],
    runtime: ProfilingRuntimeConfig,
    started_at: float,
    ended_at: float,
    status: str,
    dry_run: bool,
    error: str | None = None,
) -> dict[str, Any]:
    """构造采集摘要。

    manifest 只描述采集结果路径和采集开关，不输出 modeling 结论。
    """

    trace_dir = run_dir / "trace"
    python_probe_dir = trace_dir / "python_probe"
    torch_dir = runtime.output_path(run_dir, "torch")
    hook_prefix = runtime.output_path(run_dir, "ld_preload")
    torch_trace_files = _glob_files(torch_dir, "**/trace_view.json")
    for entry in torch_trace_files:
        clock = read_host_clock(Path(entry["path"]))
        if clock is not None:
            entry["host_clock"] = clock
    ld_preload_trace_files = _glob_files(hook_prefix.parent, escape(hook_prefix.name) + ".rank*.pid*.json")
    python_probe_files = _glob_files(python_probe_dir, "*.json")
    collection_errors = [error] if error else []
    return {
        "experiment_id": cfg.get("run_id") or cfg.get("name"),
        "framework": str(cfg.get("framework") or "sglang"),
        "name": cfg.get("name"),
        "run_id": cfg.get("run_id"),
        "run_dir": str(run_dir),
        "config_path": str(run_dir / "config.json"),
        "status": status,
        "profiling_ready": status in {"completed", "dry_run"} and not collection_errors,
        "dry_run": dry_run,
        "started_at": started_at,
        "ended_at": ended_at,
        "duration_sec": max(0.0, ended_at - started_at),
        "collection_errors": collection_errors,
        "profiling": runtime.to_manifest_fragment(),
        "trace_channel_coverage": {
            "torch_trace_files": len(torch_trace_files),
            "ld_preload_trace_files": len(ld_preload_trace_files),
            "python_probe_trace_files": len(python_probe_files),
        },
        "trace": {
            "root": str(trace_dir),
            "torch_trace_dir": str(torch_dir),
            "torch_trace_files": torch_trace_files,
            "ld_preload_trace_dir": str(hook_prefix.parent),
            "ld_preload_trace_files": ld_preload_trace_files,
        },
        "bench": {
            "workload_report_files": _glob_files(run_dir / "bench", "**/workload_report.json"),
            "bench_serving_files": _glob_files(run_dir / "bench", "**/*.jsonl"),
        },
        "sidecar": {
            "python_probe_dir": str(python_probe_dir),
            "python_probe_files": python_probe_files,
        },
    }


def _glob_files(path: Path, pattern: str) -> list[dict[str, Any]]:
    """按 glob 收集 trace 文件，并转换成 manifest 路径条目。"""

    return [
        {"path": str(item), "exists": True, "bytes": item.stat().st_size}
        for item in sorted(path.glob(pattern))
        if item.is_file()
    ]
