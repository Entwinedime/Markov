"""Retire raw traces created by this successful performance-maintenance call only."""

from __future__ import annotations

from pathlib import Path
import shutil
import time
from typing import Any

from markov_internal.common.io import load_json, write_json_atomic
from markov_internal.common.paths import ROOT_DIR, repo_relative_path, require_repo_path


def _referenced_paths(value: Any) -> list[Path]:
    if isinstance(value, list):
        return [path for item in value for path in _referenced_paths(item)]
    if isinstance(value, dict):
        if isinstance(value.get("path"), str):
            return [require_repo_path(value["path"]).resolve()]
        return [path for item in value.values() for path in _referenced_paths(item)]
    return []


def retire_performance_traces(evidence: dict[Path, tuple[str, Path]]) -> dict[str, Any]:
    """The caller supplies only newly created captures after every consumer succeeded.

    Publish the evidence reference before removing raw files, so interruption
    still leaves a usable measurement source. Custom/shared trace locations
    remain intact. Base and manually supplied historical captures never enter.
    """

    started = time.monotonic()
    results = []
    for path, (kind, retained) in evidence.items():
        manifest = load_json(path)
        run = require_repo_path(manifest["run_dir"]).resolve()
        raw = require_repo_path(manifest["trace"]["root"])
        resolved = raw.resolve()
        channels = [manifest["trace"].get(key) for key in ("torch_trace_dir", "ld_preload_trace_dir")]
        channels.append(manifest.get("sidecar", {}).get("python_probe_dir"))
        protected = [path.resolve(), require_repo_path(manifest["config_path"]).resolve(), retained.resolve()]
        protected.extend(_referenced_paths(manifest.get("bench", {})))
        owned = (
            path.resolve().parent == run
            and run.is_relative_to(ROOT_DIR.resolve())
            and resolved.is_relative_to(run)
            and resolved != run
            and not raw.is_symlink()
            and all(
                not channel or require_repo_path(channel).resolve().is_relative_to(resolved) for channel in channels
            )
            and not any(value.is_relative_to(resolved) for value in protected)
        )
        if not owned:
            results.append({"manifest": str(repo_relative_path(path)), "status": "retained_unmanaged_trace_location"})
            continue
        if manifest["status"] != "completed" or manifest.get("collection_errors"):
            raise ValueError("Only completed new performance captures may retire raw traces")
        if not retained.is_file():
            raise FileNotFoundError(retained)

        manifest["retained_performance_evidence"] = {"kind": kind, "path": str(repo_relative_path(retained))}
        if kind == "operation_samples":
            manifest["performance_measurements"] = str(repo_relative_path(retained))
        write_json_atomic(path, manifest)
        size = sum(item.lstat().st_size for item in resolved.rglob("*") if item.is_file() or item.is_symlink())
        try:
            shutil.rmtree(resolved)
        except OSError as error:
            results.append({"manifest": str(repo_relative_path(path)), "status": "removal_failed", "error": str(error)})
            continue

        manifest["raw_trace_retained"] = False
        manifest["raw_trace_logical_bytes_removed"] = size
        write_json_atomic(path, manifest)
        results.append(
            {
                "manifest": str(repo_relative_path(path)),
                "status": "removed",
                "raw_logical_bytes_removed": size,
                "evidence": str(repo_relative_path(retained)),
                "evidence_bytes": retained.stat().st_size,
            }
        )

    return {
        "status": "completed" if all(row["status"] == "removed" for row in results) else "partial",
        "scope": "new captures only; logical file bytes, not filesystem physical allocation; shared database/package bytes are separate",
        "captures": results,
        "wall_seconds": time.monotonic() - started,
    }
