"""Minimal UTF-8 JSON I/O shared by internal workflows."""

from __future__ import annotations

import json
import gzip
from collections.abc import Iterator
from pathlib import Path
from typing import Any


def load_json(path: Path) -> Any:
    """Load and decode one UTF-8 JSON file."""

    return json.loads(path.read_text(encoding="utf-8"))


def iter_json_lines(path: Path) -> Iterator[dict[str, Any]]:
    """Read one measurement at a time, including retained gzip streams."""

    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rt", encoding="utf-8") as source:
        for line in source:
            yield json.loads(line)


def write_json(path: Path, value: Any) -> None:
    """Write deterministic indented JSON after creating the parent directory."""

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def write_json_atomic(path: Path, value: Any) -> None:
    """Publish a progress report without exposing a half-written JSON on interruption."""

    pending = path.with_name(path.name + ".pending")
    try:
        write_json(pending, value)
        pending.replace(path)
    finally:
        pending.unlink(missing_ok=True)


def truncate_log_tail(path: Path, limit: int) -> None:
    """Retain at most ``limit`` trailing bytes from one failed command log."""

    if not path.is_file() or path.stat().st_size <= limit:
        return
    marker = f"[model log truncated to final {limit} bytes]\n".encode()
    tail_bytes = max(0, limit - len(marker))
    with path.open("rb") as log_file:
        log_file.seek(-tail_bytes, 2)
        tail = log_file.read()
    path.write_bytes(marker + tail)
