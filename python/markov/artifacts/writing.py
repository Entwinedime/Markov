"""Publish small owned artifacts only after their complete JSON is written."""

import json
from pathlib import Path
import tempfile


def write_json(path: Path, value) -> None:
    stream = tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=path.parent, prefix=f".{path.name}.", suffix=".partial", delete=False
    )
    pending = Path(stream.name)
    try:
        with stream:
            json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write("\n")
        pending.replace(path)
    finally:
        pending.unlink(missing_ok=True)
