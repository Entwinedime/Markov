"""validation diagnostics 使用的 predicted HiCache state record 工具。"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from ....common.io import load_json


def load_predicted_state_records(path: Path | None) -> list[dict[str, Any]]:
    """读取 C++ HiCache state model 输出的 transition 明细。

    缺失或损坏的文件只代表该次验证没有 transition 明细，不应让上层
    profiling/modeling 编排因为诊断附件不可用而失败。
    """

    if path is None or not path.is_file():
        return []
    try:
        payload = load_json(path)
    except json.JSONDecodeError:
        return []
    records = payload.get("records") if isinstance(payload, dict) else []
    return [record for record in records if isinstance(record, dict)] if isinstance(records, list) else []


def page_set_from_predicted_record(record: dict[str, Any]) -> list[Any]:
    """读取 C++ transition 明细中的目标页集合。"""

    pages = record.get("target_page_set")
    return pages if isinstance(pages, list) else []


def count_records_by_key(rows: list[dict[str, Any]], key: str) -> dict[str, int]:
    """按指定字段统计记录数量，忽略空字段值。"""

    counts: dict[str, int] = {}
    for row in rows:
        value = str(row.get(key) or "")
        if not value:
            continue
        counts[value] = counts.get(value, 0) + 1
    return dict(sorted(counts.items()))
