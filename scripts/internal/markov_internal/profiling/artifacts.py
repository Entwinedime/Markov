"""Writers for reproducible profiling-run artifacts."""

from __future__ import annotations

import time
from pathlib import Path
from typing import Any

from ..common.commands import command_to_text
from ..common.io import write_json
from ..common.paths import prepend_repo_src_to_sys_path
from .drain import capture_tail_policy
from .probe_targets import select_python_probe_targets

prepend_repo_src_to_sys_path()

from profiling import build_profile_manifest  # noqa: E402


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
    runtime: Any,
    *,
    started_at: float,
    status: str,
    dry_run: bool,
    error: str | None = None,
    storage_cleanup: dict[str, Any] | None = None,
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
    manifest["profiling"]["python_target_contract"] = _python_target_contract(runtime)
    consumers = {str(consumer) for consumer in runtime.python_consumers}
    manifest["profiling"]["capture_tail_contract"] = {
        **capture_tail_policy(cfg),
        "semantic_lifecycle_closure_required_downstream": "hicache_input_contract" in consumers,
    }
    if storage_cleanup is not None:
        manifest["profiling"]["hicache_storage_cleanup"] = storage_cleanup
    write_json(run_dir / "profile_manifest.json", manifest)


def _python_target_contract(runtime: Any) -> dict[str, Any] | None:
    """Persist the exact catalog subset installed into the profiled server."""

    if "python_probe" not in runtime.channels:
        return None
    targets = select_python_probe_targets(
        tuple(runtime.python_consumers),
        diagnostics=runtime.python_diagnostics,
    )
    return {
        "selected_target_count": len(targets),
        "selected_target_ids": [str(target["id"]) for target in targets],
        "diagnostics": runtime.python_diagnostics,
    }
