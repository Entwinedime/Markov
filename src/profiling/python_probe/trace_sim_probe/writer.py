from __future__ import annotations

import atexit
import json
import os
import threading
import time
from pathlib import Path
from typing import Any, Dict, Optional


def _truthy(value: Optional[str]) -> bool:
    return value is not None and value.lower() not in ("", "0", "false", "no", "off")


def _jsonable(value: Any) -> Any:
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, (list, tuple)):
        return [_jsonable(v) for v in value[:32]]
    if isinstance(value, dict):
        return {str(k): _jsonable(v) for k, v in list(value.items())[:64]}
    return str(value)


class ChromeTraceWriter:
    def __init__(self, output_dir: Optional[str] = None) -> None:
        self.pid = os.getpid()
        self.rank = os.environ.get("RANK", os.environ.get("LOCAL_RANK", "unknown"))
        root = Path(output_dir or os.environ.get("TRACE_SIM_PYTHON_PROBE_OUTPUT", "."))
        root.mkdir(parents=True, exist_ok=True)
        self.path = root / f"python_probe_trace.rank{self.rank}.pid{self.pid}.json"
        self._events = []
        self._lock = threading.Lock()
        atexit.register(self.close)

    @staticmethod
    def now_us() -> int:
        return time.time_ns() // 1000

    def duration_event(self, name: str, start_us: int, end_us: int, cat: str, args: Optional[Dict[str, Any]] = None) -> None:
        tid = threading.get_native_id() if hasattr(threading, "get_native_id") else threading.get_ident()
        event = {
            "name": name,
            "cat": cat,
            "ph": "X",
            "ts": int(start_us),
            "dur": max(0, int(end_us - start_us)),
            "pid": self.pid,
            "tid": tid,
            "args": _jsonable(args or {}),
        }
        with self._lock:
            self._events.append(event)
            self._write_locked()

    def close(self) -> None:
        with self._lock:
            self._write_locked()

    def _write_locked(self) -> None:
        events = list(self._events)
        tmp_path = self.path.with_suffix(self.path.suffix + ".tmp")
        tmp_path.write_text(json.dumps({"traceEvents": events}, ensure_ascii=True, separators=(",", ":")), encoding="utf-8")
        tmp_path.replace(self.path)


_writer: Optional[ChromeTraceWriter] = None
_writer_lock = threading.Lock()


def get_writer() -> ChromeTraceWriter:
    global _writer
    with _writer_lock:
        if _writer is None:
            _writer = ChromeTraceWriter()
        return _writer


def probe_debug_enabled() -> bool:
    return _truthy(os.environ.get("TRACE_SIM_PYTHON_PROBE_DEBUG"))
