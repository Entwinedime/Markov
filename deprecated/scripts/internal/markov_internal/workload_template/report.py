"""Latency summaries for JSON manual workloads."""

from __future__ import annotations

import statistics
from typing import Any, Iterable, Mapping


def latency_summary(rows: Iterable[Mapping[str, Any]]) -> dict[str, Any]:
    """Summarize request latency facts without inventing cache-state outcomes."""

    request_rows = [row for row in rows if row.get("kind") == "request"]
    latencies = [float(row["latency_ms"]) for row in request_rows if isinstance(row.get("latency_ms"), (int, float))]
    ok_count = sum(1 for row in request_rows if row.get("status") == "ok")
    result: dict[str, Any] = {
        "requests": len(request_rows),
        "ok": ok_count,
        "errors": len(request_rows) - ok_count,
        "latency_ms_sum": sum(latencies),
    }
    if latencies:
        result.update(
            {
                "latency_ms_mean": statistics.fmean(latencies),
                "latency_ms_min": min(latencies),
                "latency_ms_max": max(latencies),
                "latency_ms_p50": statistics.median(latencies),
            }
        )
    return result
