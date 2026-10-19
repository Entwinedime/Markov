"""Record Triton specialization and launcher initialization on the native path."""

from __future__ import annotations

import json
import os
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any


class CompilationObservation:
    def __init__(self, directory: Path) -> None:
        self.directory = directory
        directory.mkdir(parents=True, exist_ok=True)

    def compile(self, original: Callable[..., Any], kernel: Any, *args: Any, **kwargs: Any) -> Any:
        return self._invoke(original, kernel, "specialization", kernel.fn.__name__, args, kwargs)

    def initialize(self, original: Callable[..., Any], kernel: Any, *args: Any, **kwargs: Any) -> Any:
        # Triton calls this entry again after initialization. Only the transition
        # from an absent module builds a launcher and loads the native binary.
        if kernel.module is not None:
            return original(kernel, *args, **kwargs)
        return self._invoke(original, kernel, "launcher", kernel.name, args, kwargs)

    def _invoke(
        self,
        original: Callable[..., Any],
        kernel: Any,
        kind: str,
        name: str,
        args: tuple[Any, ...],
        kwargs: dict[str, Any],
    ) -> Any:
        started_ns = time.perf_counter_ns()
        result = original(kernel, *args, **kwargs)
        ended_ns = time.perf_counter_ns()

        # Only first use reaches these hooks. The native monotonic clock
        # identifies compilation inside a business window without timing kernels.
        record = {"kind": kind, "name": name, "started_ns": started_ns, "ended_ns": ended_ns}
        with (self.directory / f"{os.getpid()}.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(record) + "\n")
        return result

    @staticmethod
    def windows(directory: Path, windows: list[dict[str, int]]) -> dict[str, Any]:
        events = []
        for path in sorted(directory.glob("*.jsonl")):
            with path.open(encoding="utf-8") as stream:
                events.extend({**json.loads(line), "pid": int(path.stem)} for line in stream)

        in_window = [
            event
            for event in events
            if any(
                event["started_ns"] < window["ended_ns"] and event["ended_ns"] > window["started_ns"]
                for window in windows
            )
        ]
        return {
            "status": "steady" if not in_window else "first_use_in_window",
            "observer": "triton_specialization_and_launcher",
            "windows": windows,
            "observed_processes": sorted({event["pid"] for event in events}),
            "first_uses_outside_windows": len(events) - len(in_window),
            "events": in_window,
        }
