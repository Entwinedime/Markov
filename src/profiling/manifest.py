"""Profile manifest 生成。"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from .config import ProfilingRuntimeConfig


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
    torch_trace_files = _glob_files(trace_dir / "torch", "**/trace_view.json")
    ld_preload_trace_files = _glob_files(trace_dir / "ld_preload", "*")
    python_probe_files = _glob_files(python_probe_dir, "*.json")
    collection_errors = [error] if error else []
    return {
        "experiment_id": cfg.get("run_id") or cfg.get("name"),
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
            "torch_trace_dir": str(trace_dir / "torch"),
            "torch_trace_files": torch_trace_files,
            "ld_preload_trace_dir": str(trace_dir / "ld_preload"),
            "ld_preload_trace_files": ld_preload_trace_files,
        },
        "sidecar": {
            "python_probe_dir": str(python_probe_dir),
            "python_probe_files": python_probe_files,
            "python_probe_debug_files": _glob_files(python_probe_dir, "*debug*"),
        },
    }


def _path_info(path: Path) -> dict[str, Any]:
    """生成单个 trace 文件的存在性和大小摘要。"""

    return {
        "path": str(path),
        "exists": path.exists(),
        "bytes": path.stat().st_size if path.is_file() else None,
    }


def _glob_files(path: Path, pattern: str) -> list[dict[str, Any]]:
    """按 glob 收集 trace 文件，并转换成 manifest 路径条目。"""

    return [_path_info(item) for item in sorted(path.glob(pattern)) if item.is_file()]
