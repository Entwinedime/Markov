"""Read an owned Docker handle without confusing an unavailable daemon with absence."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess


def container_state(cid: Path) -> dict | None:
    if not cid.exists():
        return None
    result = subprocess.run(
        ["docker", "inspect", "--type", "container", "--format", "{{json .State}}", cid.read_text().strip()],
        capture_output=True,
        text=True,
        timeout=20,
    )
    if result.returncode == 0:
        return json.loads(result.stdout)
    if "No such container:" in result.stderr:
        return None
    raise OSError(result.stderr.strip())
