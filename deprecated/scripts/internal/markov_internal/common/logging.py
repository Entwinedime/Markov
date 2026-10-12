"""Timestamped console logging for long-running internal commands."""

from __future__ import annotations

import time


def log(message: str) -> None:
    """Emit one immediately flushed runner log line with wall-clock time."""

    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{timestamp}] {message}", flush=True)
