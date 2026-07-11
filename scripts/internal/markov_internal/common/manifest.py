"""Helpers for resolving file entries stored in profile manifests."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from .paths import map_repo_path


def existing_manifest_files(entries: Any) -> list[Path]:
    """Resolve manifest file entries that currently exist in this checkout.

    A manifest entry may be a path string or an object containing ``path`` and an
    optional ``exists`` flag. Container paths are projected into the active host
    checkout before existence is checked.
    """

    if not isinstance(entries, list):
        return []

    paths: list[Path] = []
    for entry in entries:
        if isinstance(entry, dict):
            if entry.get("exists", True) is False:
                continue
            raw_path = entry.get("path")
        else:
            raw_path = entry
        if not isinstance(raw_path, str) or not raw_path:
            continue
        path = map_repo_path(Path(raw_path))
        if path.is_file():
            paths.append(path)
    return sorted(set(paths))
