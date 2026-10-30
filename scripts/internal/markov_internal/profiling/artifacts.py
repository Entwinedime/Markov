"""Writers for reproducible profiling-run artifacts."""

from __future__ import annotations

import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from ..common.commands import command_to_text
from ..common.io import write_json
from ..common.paths import prepend_repo_src_to_sys_path
from ..common.process import container_peak_memory_bytes

prepend_repo_src_to_sys_path()

from profiling import build_profile_manifest  # noqa: E402

if TYPE_CHECKING:
    from profiling.config import ProfilingRuntimeConfig


def write_run_inputs(
    run_dir: Path,
    cfg: dict[str, Any],
    server_command: list[str] | str,
    bench_command: list[str] | str | None,
) -> None:
    """Persist the source config and expanded commands needed to reproduce a run."""

    write_json(run_dir / "config.json", cfg)
    (run_dir / "server_cmd.txt").write_text(
        command_to_text(server_command) + "\n",
        encoding="utf-8",
    )
    if bench_command is not None:
        (run_dir / "bench_cmd.txt").write_text(
            command_to_text(bench_command) + "\n",
            encoding="utf-8",
        )


def write_profile_manifest(
    run_dir: Path,
    cfg: dict[str, Any],
    runtime: ProfilingRuntimeConfig,
    *,
    python_targets: list[dict[str, Any]],
    started_at: float,
    status: str,
    dry_run: bool,
    error: str | None = None,
    storage_cleanup: dict[str, Any] | None = None,
    workload_started: bool | None = None,
    wall_timing: dict[str, float] | None = None,
) -> None:
    """Write the profile manifest consumed by downstream C++ modeling."""

    manifest = build_profile_manifest(
        run_dir=run_dir,
        cfg=cfg,
        runtime=runtime,
        started_at=started_at,
        ended_at=time.time(),
        status=status,
        dry_run=dry_run,
        error=error,
    )
    manifest["profiling"]["python_target_contract"] = (
        {
            "selected_target_count": len(python_targets),
            "selected_target_ids": [target["id"] for target in python_targets],
            "diagnostics": runtime.python_diagnostics,
        }
        if runtime.enabled and "python_probe" in runtime.channels
        else None
    )
    if workload_started is not None:
        manifest["workload_started"] = workload_started
    if storage_cleanup is not None:
        manifest["profiling"]["hicache_storage_cleanup"] = storage_cleanup
    if wall_timing is not None:
        manifest["performance"] = {
            **wall_timing,
            "container_peak_memory_bytes": container_peak_memory_bytes(),
            "post_export_trace_logical_bytes": sum(
                item.stat().st_size for item in (run_dir / "trace").rglob("*") if item.is_file()
            ),
            "scope": "non-overlapping run stages; startup may compile/load; workload includes preparation and formal requests; post-export size covers run_dir/trace only, excludes external roots and is not a sampled transient peak",
        }
    write_json(run_dir / "profile_manifest.json", manifest)
