"""One atomic JSON publication boundary for collector-owned documents."""

import json
from pathlib import Path
from typing import Any


def write_json(path: Path, value: Any, *, indent: int | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w") as stream:
        json.dump(value, stream, indent=indent, separators=(",", ":") if indent is None else None, allow_nan=False)
        stream.write("\n")

    # Publish after the complete document is written and the stream is closed.
    temporary.replace(path)
