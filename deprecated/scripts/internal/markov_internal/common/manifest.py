"""Helpers for resolving file entries stored in profile manifests."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from .paths import map_repo_path, require_repo_path


def profile_labels(config: dict[str, Any], fallback: str = "") -> tuple[str, str]:
    """Return config/workload labels; matrix labels precede ordinary metadata.

    Labels select and display inputs, not model parameters. Ordinary configs
    use their name or id without requiring matrix metadata. A manifest reader
    can supply its run identity for a config that declares neither.
    """
    metadata = config.get("metadata", {})
    identity = str(config.get("name") or config.get("id") or fallback)

    return (
        str(metadata.get("suite_server_id") or metadata.get("config_id") or identity),
        str(metadata.get("suite_input_id") or metadata.get("workload_id") or identity),
    )


def workload_report_path(manifest: dict[str, Any]) -> Path:
    """Resolve the one declared workload report required by HiCache workflows.

    Never discover reports by directory layout or ignore a missing declared file;
    its reader reports that failure rather than substituting another artifact.
    """

    reports = manifest["bench"]["workload_report_files"]
    if len(reports) != 1:
        raise ValueError(f"expected one manifest-declared workload report, found {len(reports)}")
    return require_repo_path(reports[0]["path"])


def manifest_files(entries: list[str | dict[str, Any]]) -> list[Path]:
    """Resolve all declared files without silently filtering missing assets.

    A declared unavailable entry is invalid, as in the C++ reader. Identity
    readers need only paths; consumers check current availability when used.
    """
    paths = set()
    for entry in entries:
        if isinstance(entry, dict) and entry.get("exists", True) is not True:
            raise ValueError(f"manifest declares an unavailable file: {entry['path']}")
        paths.add(map_repo_path(Path(entry["path"] if isinstance(entry, dict) else entry)))
    return sorted(paths)
