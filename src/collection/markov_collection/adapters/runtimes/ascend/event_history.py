"""Reconstruct observed event records at the native journal's entry boundary."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any


class EventHistory:
    def __init__(self, directory: Path, reference: str) -> None:
        self.reference = reference
        self.records = []
        boundaries = []
        with (directory / reference).open(encoding="utf-8") as stream:
            for line_number, line in enumerate(stream, start=1):
                record = json.loads(line)
                record["source"] = {"path": reference, "line": line_number}
                if "window_begin_ns" in record:
                    boundaries.append(record)
                else:
                    self.records.append(record)

        if len(boundaries) != 1:
            raise ValueError(f"event history requires one entry boundary: {reference}")
        self.boundary = boundaries[0]

    def initial_state(self) -> dict[str, Any]:
        active = {}
        previous_ends = {}
        gaps = []
        for record in sorted(self.records, key=lambda item: item["start_ns"]):
            if record["pid"] != self.boundary["pid"]:
                raise ValueError(f"event history mixes worker identities: {self.reference}")

            event = record["event_handle"]
            if record["result"] != 0:
                gaps.append({"reason": "initial_event_api_failed", "source": record["source"]})
                continue
            if record["end_ns"] > self.boundary["window_begin_ns"]:
                # A call that straddles entry is not an already-completed state
                # transition. Preserve the ambiguity instead of shifting it earlier.
                gaps.append({"reason": "event_call_crosses_window_entry", "source": record["source"]})
                continue
            if event in previous_ends and record["start_ns"] < previous_ends[event]:
                gaps.append({"reason": "initial_event_calls_overlap", "source": record["source"]})
            previous_ends[event] = record["end_ns"]

            if record["api"] == "rtEventRecord":
                active[event] = {
                    "event_handle": event,
                    "stream_handle": record["stream_handle"],
                    "stream_id": record["stream_id"],
                    "record_source": record["source"],
                    "observed_status": None,
                    "query_source": None,
                }
            elif record["api"] == "rtEventDestroy":
                active.pop(event, None)
            elif record["api"] == "rtEventQueryStatus":
                # New, unrecorded events may already query as complete. A query
                # cannot manufacture a producer or a live record for such a handle.
                if event in active:
                    active[event]["observed_status"] = record["event_status"]
                    active[event]["query_source"] = record["source"]
            else:
                raise ValueError(f"unexpected API in event entry history: {record['api']}")

        return {
            "pid": self.boundary["pid"],
            "window_begin_ns": self.boundary["window_begin_ns"],
            "source": self.boundary["source"],
            "scope": "recorded event lifecycle only; excludes device completion and watchdog queues",
            "events": list(active.values()),
            "gaps": gaps,
        }
