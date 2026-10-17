"""Atomic publication of collection artifacts."""

import gzip
import json
from pathlib import Path
from typing import Any


def read_json(path: Path) -> Any:
    stream = gzip.open(path, "rt", encoding="utf-8") if path.suffix == ".gz" else path.open(encoding="utf-8")
    with stream:
        return json.load(stream)


def write_json(path: Path, value: Any, *, compact: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    stream = (
        gzip.open(temporary, "wt", encoding="utf-8", compresslevel=1)
        if path.suffix == ".gz"
        else temporary.open("w", encoding="utf-8")
    )
    with stream:
        json.dump(value, stream, indent=None if compact else 2, separators=(",", ":") if compact else None)
        stream.write("\n")

    temporary.replace(path)
