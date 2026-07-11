"""Predicted HiCache state records consumed by validation diagnostics."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from markov_internal.common.io import load_json


def load_predicted_state_records(path: Path | None) -> list[dict[str, Any]]:
    """Load transition details emitted by the C++ HiCache state model.

    A missing or malformed optional attachment yields no diagnostic records;
    it does not abort the higher-level profiling or modeling orchestration.
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
    """Return the target page set carried by a predicted transition."""

    pages = record.get("target_page_set")
    return pages if isinstance(pages, list) else []


def count_records_by_key(rows: list[dict[str, Any]], key: str) -> dict[str, int]:
    """Count records by a non-empty field value."""

    counts: dict[str, int] = {}
    for row in rows:
        value = str(row.get(key) or "")
        if not value:
            continue
        counts[value] = counts.get(value, 0) + 1
    return dict(sorted(counts.items()))
