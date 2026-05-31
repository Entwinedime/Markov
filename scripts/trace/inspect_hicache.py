#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional


HICACHE_KEYS = (
    "framework",
    "request_id",
    "op_id",
    "tier_src",
    "tier_dst",
    "direction",
    "pool",
    "num_tokens",
    "num_pages",
    "page_size",
    "bytes",
    "bytes_per_page",
    "layout",
    "io_backend",
    "storage_backend",
    "write_policy",
    "status",
)

NUMERIC_KEYS = ("num_tokens", "num_pages", "page_size", "bytes", "bytes_per_page")


def load_events(path: Path) -> List[Dict[str, Any]]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return []
    if isinstance(data, dict):
        events = data.get("traceEvents", [])
    else:
        events = data
    return [event for event in events if isinstance(event, dict)]


def is_hicache_event(event: Dict[str, Any]) -> bool:
    args = event.get("args", {})
    if not isinstance(args, dict):
        args = {}
    name = str(event.get("name", ""))
    return event.get("cat") == "hicache" or name.startswith("HiCache::") or args.get("domain") == "cache_io"


def discover_trace_files(path: Path) -> List[Path]:
    if path.is_file():
        return [path]
    if not path.is_dir():
        return []

    merged_patterns = (
        "trace/merged/merged_trace.pid*.json",
        "**/trace/merged/merged_trace.pid*.json",
    )
    probe_patterns = (
        "trace/python_probe/python_probe_trace.rankunknown.pid*.json",
        "trace/python_probe/python_probe_trace.rank*.pid*.json",
        "**/trace/python_probe/python_probe_trace.rankunknown.pid*.json",
        "**/trace/python_probe/python_probe_trace.rank*.pid*.json",
    )

    for patterns in (merged_patterns, probe_patterns):
        files: List[Path] = []
        seen = set()
        for pattern in patterns:
            for candidate in sorted(path.glob(pattern)):
                if candidate.is_file() and candidate not in seen:
                    seen.add(candidate)
                    files.append(candidate)
        if files:
            return files

    patterns = ("*.json",)
    files: List[Path] = []
    seen = set()
    for pattern in patterns:
        for candidate in sorted(path.glob(pattern)):
            if candidate.is_file() and candidate not in seen:
                seen.add(candidate)
                files.append(candidate)
    return files


def as_positive_number(value: Any) -> Optional[float]:
    if value is None:
        return None
    try:
        numeric = float(value)
    except (TypeError, ValueError):
        return None
    return numeric if numeric > 0 else None


def edge_name(args: Dict[str, Any]) -> str:
    src = args.get("tier_src") or ""
    dst = args.get("tier_dst") or ""
    if not src and not dst:
        return "unknown"
    return f"{src}->{dst}" if dst else f"{src}->"


def summarize(paths: Iterable[Path], sample_limit: int = 5) -> Dict[str, Any]:
    files = []
    events_by_name: Counter[str] = Counter()
    events_by_pid: Counter[str] = Counter()
    status_counts: Counter[str] = Counter()
    direction_counts: Counter[str] = Counter()
    edge_counts: Counter[str] = Counter()
    storage_backend_counts: Counter[str] = Counter()
    key_present: Counter[str] = Counter()
    numeric_nonzero: Counter[str] = Counter()
    sample_events: List[Dict[str, Any]] = []
    total_trace_events = 0
    hicache_events = 0
    transfer_events = 0
    min_ts: Optional[float] = None
    max_ts: Optional[float] = None

    for path in paths:
        events = load_events(path)
        total_trace_events += len(events)
        file_hicache = 0
        for event in events:
            if not is_hicache_event(event):
                continue

            args = event.get("args", {})
            if not isinstance(args, dict):
                args = {}

            file_hicache += 1
            hicache_events += 1
            name = str(event.get("name", "unknown"))
            events_by_name[name] += 1
            events_by_pid[str(event.get("pid", "unknown"))] += 1
            status_counts[str(args.get("status", "missing"))] += 1
            direction_counts[str(args.get("direction", "missing"))] += 1
            edge = edge_name(args)
            edge_counts[edge] += 1
            if "->" in edge and not edge.endswith("->") and edge != "unknown":
                transfer_events += 1
            storage_backend_counts[str(args.get("storage_backend", "missing"))] += 1

            for key in HICACHE_KEYS:
                value = args.get(key)
                if value not in (None, ""):
                    key_present[key] += 1
            for key in NUMERIC_KEYS:
                if as_positive_number(args.get(key)) is not None:
                    numeric_nonzero[key] += 1

            ts = as_positive_number(event.get("ts"))
            dur = as_positive_number(event.get("dur")) or 0.0
            if ts is not None:
                min_ts = ts if min_ts is None else min(min_ts, ts)
                max_ts = ts + dur if max_ts is None else max(max_ts, ts + dur)

            if len(sample_events) < sample_limit:
                sample_events.append(
                    {
                        "name": name,
                        "ts": event.get("ts"),
                        "dur": event.get("dur"),
                        "pid": event.get("pid"),
                        "tid": event.get("tid"),
                        "args": {key: args.get(key) for key in HICACHE_KEYS if key in args},
                    }
                )

        files.append({"path": str(path), "trace_events": len(events), "hicache_events": file_hicache})

    key_coverage = {
        key: {
            "present": key_present.get(key, 0),
            "missing": max(0, hicache_events - key_present.get(key, 0)),
        }
        for key in HICACHE_KEYS
    }
    numeric_coverage = {
        key: {
            "nonzero": numeric_nonzero.get(key, 0),
            "zero_or_missing": max(0, hicache_events - numeric_nonzero.get(key, 0)),
        }
        for key in NUMERIC_KEYS
    }

    return {
        "files": files,
        "total_files": len(files),
        "total_trace_events": total_trace_events,
        "hicache_events": hicache_events,
        "transfer_events": transfer_events,
        "time_range_us": None if min_ts is None or max_ts is None else [min_ts, max_ts],
        "events_by_name": dict(events_by_name),
        "events_by_pid": dict(events_by_pid),
        "status_counts": dict(status_counts),
        "direction_counts": dict(direction_counts),
        "edge_counts": dict(edge_counts),
        "storage_backend_counts": dict(storage_backend_counts),
        "key_coverage": key_coverage,
        "numeric_coverage": numeric_coverage,
        "sample_events": sample_events,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Inspect HiCache/cache_io events from trace files or a profile run directory.")
    parser.add_argument("path", help="Trace JSON file, experiment dir, or profile run dir")
    parser.add_argument("--output", "-o", help="Write summary JSON to this path")
    parser.add_argument("--samples", type=int, default=5, help="Number of sample events to include")
    args = parser.parse_args()

    paths = discover_trace_files(Path(args.path))
    report = summarize(paths, sample_limit=max(0, args.samples))
    payload = json.dumps(report, indent=2, sort_keys=True)
    if args.output:
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(payload + "\n", encoding="utf-8")
    else:
        print(payload)
    return 0 if report["hicache_events"] > 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
