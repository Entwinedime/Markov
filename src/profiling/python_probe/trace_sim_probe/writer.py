"""Python probe Chrome trace writer。"""

from __future__ import annotations

import atexit
import json
import os
import threading
import time
from pathlib import Path
from typing import Any


def _truthy(value: str | None) -> bool:
    """解析 probe 环境变量中的宽松布尔值。"""

    return value is not None and value.lower() not in ("", "0", "false", "no", "off")


_FULL_LIST_KEYS = {
    "token_ids",
    "node_chain",
    "last_node_chain",
    "last_host_node_chain",
    "best_match_node_chain",
    "operation_hash_pages",
    "hash_value",
    "hash_pages",
    "hit_hash_pages",
    "prefix_keys",
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
        self._events: list[dict[str, Any]] = []
        self._lock = threading.Lock()
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
        """追加一条 duration event，并立即落盘到临时文件再原子替换。"""

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
        """进程退出时刷新当前内存中的事件列表。"""

        with self._lock:
            self._write_locked()

    def _write_locked(self) -> None:
        """在持锁状态下写 trace 文件，避免多线程交错写 JSON。"""

        tmp_path = self.path.with_suffix(self.path.suffix + ".tmp")
        tmp_path.write_text(
            json.dumps({"traceEvents": list(self._events)}, ensure_ascii=True, separators=(",", ":")),
            encoding="utf-8",
        )
        tmp_path.replace(self.path)


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
