"""Chrome trace JSON 读取工具。

Python probe 采用流式写入后，真实 profiling 进程被外层停止时可能来不及执行
``atexit`` 收尾，导致文件缺少最后的 ``]}``。这里把这类“尾部未闭合但事件对象
完整”的情况集中修复，避免每个审计脚本各自实现一套宽松解析逻辑。
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class TraceLoadStatus:
    """单个 trace 文件的读取状态。"""

    path: str
    loaded: bool
    event_count: int
    repaired: bool = False
    error: str = ""

    def to_dict(self) -> dict[str, Any]:
        """转换为稳定 JSON 摘要。"""

        return {
            "path": self.path,
            "loaded": self.loaded,
            "event_count": self.event_count,
            "repaired": self.repaired,
            "error": self.error,
        }


def load_chrome_trace(path: Path, *, auto_repair: bool = True) -> tuple[Any, list[dict[str, Any]], TraceLoadStatus]:
    """读取 Chrome trace payload、event 列表和读取状态。"""

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
    """只读取 Chrome trace event 列表。"""

    _payload, events, status = load_chrome_trace(path, auto_repair=auto_repair)
    return events, status


def trace_events_from_payload(payload: Any) -> list[dict[str, Any]]:
    """从 Chrome trace payload 中提取 dict event。"""

    raw_events = payload.get("traceEvents", []) if isinstance(payload, dict) else payload
    if not isinstance(raw_events, list):
        return []
    return [event for event in raw_events if isinstance(event, dict)]


def repair_streamed_chrome_trace_text(text: str) -> str:
    """修复流式 Chrome trace 最常见的尾部未闭合情况。"""

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
