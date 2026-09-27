"""Chrome trace loading with one bounded repair policy.

Python probes stream events while the profiled process is running. External
shutdown can bypass ``atexit`` and leave a file without its final ``]}``, even
when every event object is complete. This module centralizes repair of that one
known truncation shape so audits do not implement inconsistent permissive JSON
parsers.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any


def load_chrome_trace_events(path: Path, *, auto_repair: bool = True) -> list[dict[str, Any]]:
    """Load events; missing files and invalid JSON raise rather than imply an empty trace.

    Only a missing container footer may be repaired. Partial events remain errors.
    """
    text = path.read_text(encoding="utf-8").strip()
    try:
        payload = json.loads(text)
    except json.JSONDecodeError:
        closed = repair_streamed_chrome_trace_text(text) if auto_repair else text
        if closed == text:
            raise
        payload = json.loads(closed)

    return trace_events_from_payload(payload)


def trace_events_from_payload(payload: Any) -> list[dict[str, Any]]:
    """Extract dictionary events from object-style or array-style payloads."""

    raw_events = payload.get("traceEvents", []) if isinstance(payload, dict) else payload
    if not isinstance(raw_events, list):
        return []
    return [event for event in raw_events if isinstance(event, dict)]


def repair_streamed_chrome_trace_text(text: str) -> str:
    """Close a streamed trace only when its final event object is complete."""

    stripped = text.strip()
    # Only append the missing container footer. Never discard a partial event
    # after the last complete object: that would turn lost data into success.
    if not stripped.endswith("}"):
        return text
    if stripped.startswith('{"traceEvents":[') and not stripped.endswith("]}"):
        return stripped + "]}"
    if stripped.startswith("["):
        return stripped + "]"
    return text
