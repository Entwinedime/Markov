"""Publish small owned artifacts only after their complete JSON is written."""

import json
from pathlib import Path


def write_json(path: Path, value) -> None:
    pending = path.with_name(path.name + ".partial")
    stream = pending.open("x", encoding="utf-8")
    try:
        with stream:
            json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write("\n")
        pending.replace(path)
    finally:
        pending.unlink(missing_ok=True)
