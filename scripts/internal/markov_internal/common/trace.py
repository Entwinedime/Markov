"""Chrome trace loading with one bounded repair policy.

Python probes stream events while the profiled process is running. External
shutdown can bypass ``atexit`` and leave a file without its final ``]}``, even
when every event object is complete. This module centralizes repair of that one
known truncation shape so audits do not implement inconsistent permissive JSON
parsers.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class TraceLoadStatus:
    """Observable outcome of loading one trace file."""

    path: str
    loaded: bool
    event_count: int
    repaired: bool = False
    error: str = ""

    def to_dict(self) -> dict[str, Any]:
        """Return the stable JSON representation used by audit artifacts."""

        return {
            "path": self.path,
            "loaded": self.loaded,
            "event_count": self.event_count,
            "repaired": self.repaired,
            "error": self.error,
        }


def load_chrome_trace(path: Path, *, auto_repair: bool = True) -> tuple[Any, list[dict[str, Any]], TraceLoadStatus]:
    """Load a Chrome trace payload, dictionary events, and status metadata."""

    if not path.is_file():
        return None, [], TraceLoadStatus(str(path), loaded=False, event_count=0, error="missing_file")

    text = path.read_text(encoding="utf-8").strip()
    try:
        payload = json.loads(text)
        events = trace_events_from_payload(payload)
        return payload, events, TraceLoadStatus(str(path), loaded=True, event_count=len(events))
    except json.JSONDecodeError as error:
        if not auto_repair:
            return None, [], TraceLoadStatus(str(path), loaded=False, event_count=0, error=str(error))
        repaired = repair_streamed_chrome_trace_text(text)
        if repaired == text:
            return None, [], TraceLoadStatus(str(path), loaded=False, event_count=0, error=str(error))

    try:
        payload = json.loads(repaired)
        events = trace_events_from_payload(payload)
        return payload, events, TraceLoadStatus(str(path), loaded=True, event_count=len(events), repaired=True)
    except json.JSONDecodeError as repair_error:
        return None, [], TraceLoadStatus(str(path), loaded=False, event_count=0, error=str(repair_error))


def load_chrome_trace_events(path: Path, *, auto_repair: bool = True) -> tuple[list[dict[str, Any]], TraceLoadStatus]:
    """Load only dictionary events plus trace status metadata."""

    _payload, events, status = load_chrome_trace(path, auto_repair=auto_repair)
    return events, status


def trace_events_from_payload(payload: Any) -> list[dict[str, Any]]:
    """Extract dictionary events from object-style or array-style payloads."""

    raw_events = payload.get("traceEvents", []) if isinstance(payload, dict) else payload
    if not isinstance(raw_events, list):
        return []
    return [event for event in raw_events if isinstance(event, dict)]


def repair_streamed_chrome_trace_text(text: str) -> str:
    """Close a streamed trace only when its final event object is complete."""

    stripped = text.strip()
    if not stripped:
        return text
    if stripped.startswith('{"traceEvents":[') and not stripped.endswith("]}"):
        end = stripped.rfind("}")
        if end >= 0:
            return stripped[: end + 1].rstrip(",") + "]}"
    if stripped.startswith("[") and not stripped.endswith("]"):
        end = stripped.rfind("}")
        if end >= 0:
            return stripped[: end + 1].rstrip(",") + "]"
    return text
