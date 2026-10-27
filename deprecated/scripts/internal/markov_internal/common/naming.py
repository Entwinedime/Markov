"""Filesystem-safe naming helpers for generated artifacts."""

from __future__ import annotations

import re


def sanitize(value: str) -> str:
    """Normalize a user-facing name into a non-empty directory component."""

    value = re.sub(r"[^A-Za-z0-9_.-]+", "_", value.strip())
    return value.strip("._-") or "profile"


def safe_slug(value: str, *, fallback: str = "unknown") -> str:
    """Normalize an internal identifier into a stable path component."""

    return re.sub(r"[^A-Za-z0-9_.-]+", "_", value.strip()).strip("._") or fallback
