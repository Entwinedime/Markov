"""Safe lifecycle cleanup for run-local HiCache backend payloads."""

from __future__ import annotations

import os
import shutil
from pathlib import Path
from typing import Any


def cleanup_run_local_hicache_storage(
    run_dir: Path,
    storage_raw: str,
    *,
    enabled: bool,
    sync_after_removal: bool = False,
) -> dict[str, Any]:
    """Remove only a storage directory nested below the current profile cell."""

    if not storage_raw:
        return {"status": "not_configured", "removed": False}
    storage_dir = Path(storage_raw).resolve()
    resolved_run_dir = run_dir.resolve()
    if not storage_dir.is_relative_to(resolved_run_dir) or storage_dir == resolved_run_dir:
        raise ValueError("HiCache storage cleanup is restricted to a child of the current run directory")
    if not enabled:
        return {
            "status": "retained_by_config",
            "removed": False,
            "path": str(storage_dir),
            "filesystem_sync_after_removal": False,
        }
    if not storage_dir.exists():
        return {
            "status": "already_absent",
            "removed": False,
            "path": str(storage_dir),
            "filesystem_sync_after_removal": False,
        }
    shutil.rmtree(storage_dir)
    # The file backend can leave several GiB of dirty pages behind a completed
    # workload.  Unlinking its files makes the payload disposable, but the
    # kernel may still finish filesystem writeback while the next matrix cell
    # is already running.  A configured sync is therefore part of the serial
    # experiment-isolation contract, not a durability requirement.
    if sync_after_removal:
        os.sync()
    return {
        "status": "removed_after_server_exit",
        "removed": True,
        "path": str(storage_dir),
        "filesystem_sync_after_removal": sync_after_removal,
    }
