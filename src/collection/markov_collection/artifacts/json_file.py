"""Atomic publication of collection artifacts."""

import json
from pathlib import Path
from typing import Any


def write_json(path: Path, value: Any, *, compact: bool = False) -> None:
    temporary = path.with_suffix(".tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=None if compact else 2, separators=(",", ":") if compact else None)
        stream.write("\n")

    temporary.replace(path)
