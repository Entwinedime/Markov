"""Resolve raw trace paths recorded by modeling validation artifacts."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.manifest import existing_manifest_files
from .trace_channels import configured_trace_channels


def trace_paths_for_run(config: dict[str, Any], manifest_path: Path) -> list[Path]:
    """Return existing manifest traces consumed by this C++ run."""

    channels = configured_trace_channels(config)
    return trace_paths_from_manifest(manifest_path, channels=set(channels) if channels is not None else None)


def trace_paths_from_manifest(manifest_path: Path, *, channels: set[str] | None = None) -> list[Path]:
    """Expand existing trace files from a profile manifest and channel filter."""

    manifest = load_json(manifest_path)
    trace = manifest.get("trace") if isinstance(manifest.get("trace"), dict) else {}
    sidecar = manifest.get("sidecar") if isinstance(manifest.get("sidecar"), dict) else {}
    paths: list[Path] = []
    if channels is None or "torch" in channels:
        paths.extend(existing_manifest_files(trace.get("torch_trace_files", [])))
    if channels is None or "ld_preload" in channels:
        paths.extend(existing_manifest_files(trace.get("ld_preload_trace_files", [])))
    if channels is None or "python_probe" in channels:
        paths.extend(existing_manifest_files(sidecar.get("python_probe_files", [])))
    return sorted(set(paths))
