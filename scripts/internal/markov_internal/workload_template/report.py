"""Artifact writing and latency summaries for JSON manual workloads."""

from __future__ import annotations

import json
import statistics
from pathlib import Path
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


def write_outputs(
    output_dir: Path,
    *,
    summary: Mapping[str, Any],
) -> None:
    """Persist the single workload report consumed by profiling and modeling."""

    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "workload_report.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
