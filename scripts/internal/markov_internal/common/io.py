"""Minimal UTF-8 JSON I/O shared by internal workflows."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any


def load_json(path: Path) -> Any:
    """Load and decode one UTF-8 JSON file."""

    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value: Any) -> None:
    """Write deterministic indented JSON after creating the parent directory."""

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def output_is_current(output_path: Path, input_paths: list[Path]) -> bool:
    """Return whether an output exists and is no older than every input."""

    try:
        output_mtime = output_path.stat().st_mtime_ns
        return all(path.is_file() and output_mtime >= path.stat().st_mtime_ns for path in input_paths)
    except OSError:
        return False
