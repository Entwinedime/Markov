"""Python probe Chrome trace writer。"""

from __future__ import annotations

import atexit
import json
import os
import threading
import time
from pathlib import Path
from typing import Any, TextIO


def _truthy(value: str | None) -> bool:
    """解析 probe 环境变量中的宽松布尔值。"""

    return value is not None and value.lower() not in ("", "0", "false", "no", "off")


_FULL_LIST_KEYS = {
    "token_ids",
    "hash_value",
}


def _jsonable(value: Any, *, key: str | None = None) -> Any:
    """把任意 Python 对象收敛成可 JSON 序列化的紧凑值。

    大列表默认截断，token/path/hash 这类建模事实字段保留完整列表。
    """

    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, (list, tuple)):
        items = value if key in _FULL_LIST_KEYS else value[:32]
        return [_jsonable(item) for item in items]
    if isinstance(value, dict):
        return {str(child_key): _jsonable(item, key=str(child_key)) for child_key, item in list(value.items())[:64]}
    return str(value)


class ChromeTraceWriter:
    """线程安全 Chrome trace writer。"""

    def __init__(self, output_dir: str | None = None) -> None:
        """初始化当前进程的 Python probe trace 文件。"""

        self.pid = os.getpid()
        self.rank = os.environ.get("RANK", os.environ.get("LOCAL_RANK", "unknown"))
        root = Path(output_dir or os.environ.get("TRACE_SIM_PYTHON_PROBE_OUTPUT", "."))
        root.mkdir(parents=True, exist_ok=True)
        self.path = root / f"python_probe_trace.rank{self.rank}.pid{self.pid}.json"
        self._flush_every = max(1, _env_u64("TRACE_SIM_PYTHON_PROBE_FLUSH_EVERY", 256))
        self._flush_interval_sec = _env_nonnegative_float(
            "TRACE_SIM_PYTHON_PROBE_FLUSH_INTERVAL_SEC",
            0.0,
        )
        self._file: TextIO = self.path.open("w", encoding="utf-8")
        self._file.write('{"traceEvents":[')
        self._first_event = True
        self._event_count = 0
        self._flushed_event_count = 0
        self._closed = False
        self._lock = threading.Lock()
        self._flush_stop = threading.Event()
        self._flush_thread: threading.Thread | None = None
        if self._flush_interval_sec > 0:
            self._flush_thread = threading.Thread(
                target=self._periodic_flush,
                name="trace-sim-python-probe-flush",
                daemon=True,
            )
            self._flush_thread.start()
        atexit.register(self.close)

    @staticmethod
    def now_us() -> int:
        """返回 Chrome trace 使用的微秒级墙钟 timestamp。"""

        return time.time_ns() // 1000

    def duration_event(
        self,
        name: str,
        start_us: int,
        end_us: int,
        cat: str,
        args: dict[str, Any] | None = None,
    ) -> None:
        """追加一条 duration event；文件按事件流写入，并按批次 flush。"""

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
            self._write_event_locked(event)

    def close(self) -> None:
        """进程退出时补齐 Chrome trace JSON 结尾并关闭文件。"""

        self._flush_stop.set()
        flush_thread = self._flush_thread
        if flush_thread is not None and flush_thread is not threading.current_thread():
            flush_thread.join(timeout=max(1.0, self._flush_interval_sec * 2.0))
        with self._lock:
            if self._closed:
                return
            self._file.write("]}\n")
            self._file.flush()
            self._file.close()
            self._closed = True

    def _write_event_locked(self, event: dict[str, Any]) -> None:
        """在持锁状态下追加单个 event，避免多线程交错写 JSON。"""

        if self._closed:
            return
        if not self._first_event:
            self._file.write(",")
        else:
            self._first_event = False
        self._file.write(json.dumps(event, ensure_ascii=True, separators=(",", ":")))
        self._event_count += 1
        if self._event_count % self._flush_every == 0:
            self._flush_locked()

    def _periodic_flush(self) -> None:
        """Flush a partial event batch at low frequency outside the hot path."""

        while not self._flush_stop.wait(self._flush_interval_sec):
            with self._lock:
                if self._closed:
                    return
                self._flush_locked()

    def _flush_locked(self) -> None:
        """Flush only when at least one event was written since the last flush."""

        if self._flushed_event_count == self._event_count:
            return
        self._file.flush()
        self._flushed_event_count = self._event_count


_writer: ChromeTraceWriter | None = None
_writer_lock = threading.Lock()


def get_writer() -> ChromeTraceWriter:
    """返回进程级 singleton writer。"""

    global _writer
    with _writer_lock:
        if _writer is None:
            _writer = ChromeTraceWriter()
        return _writer


def probe_debug_enabled() -> bool:
    """判断是否允许 probe 将调试信息写到 stderr。"""

    return _truthy(os.environ.get("TRACE_SIM_PYTHON_PROBE_DEBUG"))


def _env_u64(name: str, fallback: int) -> int:
    """读取非负整数环境变量，非法值按 fallback 处理。"""

    raw = os.environ.get(name)
    if raw is None:
        return fallback
    try:
        value = int(raw)
    except ValueError:
        return fallback
    return value if value >= 0 else fallback


def _env_nonnegative_float(name: str, fallback: float) -> float:
    """Read a non-negative finite float environment variable."""

    raw = os.environ.get(name)
    if raw is None:
        return fallback
    try:
        value = float(raw)
    except ValueError:
        return fallback
    return value if value >= 0 and value != float("inf") else fallback
