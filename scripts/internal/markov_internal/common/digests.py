"""Stable content digests shared by profiling and modeling contracts."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any


def sha256_json(value: Any) -> str:
    """Return a deterministic SHA-256 digest for a JSON-compatible value.

    The prefix records the canonicalization scheme so persisted identities cannot
    be confused with byte-for-byte file hashes.
    """

    encoded = json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return "sha256_json:" + hashlib.sha256(encoded).hexdigest()


def sha256_file(path: Path, *, chunk_size: int = 1024 * 1024) -> str:
    """Return a streaming SHA-256 digest for the exact bytes in ``path``."""

    hasher = hashlib.sha256()
    with path.open("rb") as file_obj:
        for chunk in iter(lambda: file_obj.read(chunk_size), b""):
            hasher.update(chunk)
    return "sha256_file:" + hasher.hexdigest()
