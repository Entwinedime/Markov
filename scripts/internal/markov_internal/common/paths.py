"""Repository and container path conventions for internal scripts."""

from __future__ import annotations

import os
import sys
from pathlib import Path


CONTAINER_REPO_PREFIXES = ("/workspace/trace-sim", "/opt/trace-sim")
MODELING_CONTAINER_ENV = "TRACE_SIM_MODELING_CONTAINER"


def repo_root() -> Path:
    """Locate the repository root from this module's resolved path."""

    current = Path(__file__).resolve()
    for parent in (current, *current.parents):
        if (parent / "pyproject.toml").is_file() and (parent / "scripts").is_dir():
            return parent
    raise RuntimeError(f"cannot locate repo root from {current}")


ROOT_DIR = repo_root()


def prepend_repo_src_to_sys_path() -> None:
    """Expose repository-owned ``src`` Python packages for internal scripts.

    Internal entrypoints install only ``scripts/internal`` on ``sys.path``.
    Profiling schema and manifest modules intentionally remain under ``src``,
    so callers invoke this boundary immediately before importing those modules.
    The operation is idempotent and never reorders an existing path entry.
    """

    src_dir = str(ROOT_DIR / "src")
    if src_dir not in sys.path:
        sys.path.insert(0, src_dir)


def map_repo_path(path: Path) -> Path:
    """Project a supported container repository path into this checkout."""

    raw = str(path)
    for prefix in CONTAINER_REPO_PREFIXES:
        if raw == prefix:
            return ROOT_DIR
        if raw.startswith(prefix + "/"):
            return ROOT_DIR / raw[len(prefix) + 1 :]
    return path


def resolve_repo_path(value: str | os.PathLike[str] | None) -> Path | None:
    """Resolve repository-relative, host-absolute, or container-absolute input."""

    if value is None:
        return None
    path = Path(value).expanduser()
    if path.is_absolute():
        return map_repo_path(path)
    return ROOT_DIR / path


def require_repo_path(value: str | os.PathLike[str] | None) -> Path:
    """Resolve a required repository path."""

    path = resolve_repo_path(value)
    if path is None:
        raise ValueError("expected a non-empty repository path")
    return path


def repo_relative_path(value: str | os.PathLike[str]) -> Path:
    """Return a stable repository-relative path for generated config files."""

    path = require_repo_path(value).resolve()
    try:
        return path.relative_to(ROOT_DIR.resolve())
    except ValueError as error:
        raise ValueError(f"path must be inside the repository: {path}") from error


def running_in_modeling_container() -> bool:
    """Return whether execution is inside a supported modeling container."""

    root = str(ROOT_DIR)
    under_container_repo = any(root == prefix or root.startswith(prefix + "/") for prefix in CONTAINER_REPO_PREFIXES)
    has_container_marker = os.environ.get(MODELING_CONTAINER_ENV) == "1" or Path("/.dockerenv").exists()
    return under_container_repo and has_container_marker
