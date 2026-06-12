#!/usr/bin/env python3
"""Build a diagnostic HiCache trace with oracle injections at async divergences.

This helper is deliberately outside the normal modeling path. It consumes a
divergence report produced with ``--diagnostic-inject-async`` and writes a new
trace containing synthetic ``diagnostic_state_injection`` events. Those events
let the C++ model continue from oracle-aligned state after an async boundary,
so downstream deterministic rules are re-evaluated instead of merely replayed.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from model_runner import (  # noqa: E402
    derived_hicache_state_from_snapshot,
    extract_hicache_state_snapshots,
    load_json,
    normalize_hicache_page_key,
    snapshot_logical_time_us,
)


DEFAULT_STATE_KEYS = (
    "l1_resident_pages",
    "l2_resident_pages",
    "dirty_pages",
    "backuped_pages",
    "evicted_pages",
    "locked_pages",
)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-config", type=Path, required=True)
    parser.add_argument("--base-trace", type=Path, required=True)
    parser.add_argument("--oracle-trace", type=Path, action="append", default=[])
    parser.add_argument("--divergence-report", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--page-key-mode", default="strip_scope", choices=("raw", "strip_scope"))
    parser.add_argument("--state-key", action="append", default=[])
    return parser.parse_args(argv)


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def trace_events(payload: Any) -> list[dict[str, Any]]:
    events = payload.get("traceEvents") if isinstance(payload, dict) else payload
    if not isinstance(events, list):
        raise ValueError("base trace must be a Chrome trace object or traceEvents list")
    return [event for event in events if isinstance(event, dict)]


def normalize_state_pages(state: dict[str, Any], state_keys: list[str], page_key_mode: str) -> dict[str, list[str]]:
    normalized: dict[str, list[str]] = {}
    for key in state_keys:
        raw_pages = state.get(key)
        if not isinstance(raw_pages, list):
            raw_pages = []
        normalized[key] = sorted({normalize_hicache_page_key(page, page_key_mode) for page in raw_pages if page is not None})
    return normalized


def snapshot_rows_by_order(paths: list[Path]) -> dict[int, dict[str, Any]]:
    rows = extract_hicache_state_snapshots(paths)
    return {int(row.get("order") or 0): row for row in rows}


def build_injection_event(
    injection: dict[str, Any],
    row: dict[str, Any],
    state_keys: list[str],
    page_key_mode: str,
    index: int,
) -> tuple[dict[str, Any], dict[str, Any]]:
    snapshot = row.get("state_snapshot")
    if not isinstance(snapshot, dict):
        raise ValueError(f"injection snapshot row has no state_snapshot: {row.get('order')}")
    state = normalize_state_pages(derived_hicache_state_from_snapshot(snapshot), state_keys, page_key_mode)
    compare_scope = str(injection.get("compare_scope") or "")
    ts = snapshot_logical_time_us(row) + index + 1
    event = {
        "name": "hicache_diagnostic_async_state_injection_end",
        "cat": "python_probe",
        "ph": "X",
        "ts": ts,
        "dur": 1,
        "pid": row.get("pid"),
        "tid": row.get("tid"),
        "args": {
            "schema_version": 1,
            "domain": "python_probe",
            "target_id": "hicache.diagnostic_state_injection",
            "target": "HiCacheState.diagnostic_state_injection",
            "phase": "end",
            "status": "completed",
            "model_input": True,
            "dag_input": False,
            "fact_class": "invariant_state",
            "event_role": "diagnostic_state_injection",
            "fact_granularity": "atomic",
            "cache_scope": compare_scope,
            "seq_no": 9_000_000 + index,
            "diagnostic_kind": "async_elision_oracle_injection",
            "diagnostic_source": "oracle_state_snapshot",
            "source_snapshot_order": row.get("order"),
            "source_snapshot_event_name": row.get("event_name"),
            "source_snapshot_ts": row.get("ts"),
            "async_classification": injection.get("async_classification", {}),
            "diagnostic_state": state,
        },
    }
    manifest_row = {
        "injection_index": index,
        "event_ts": ts,
        "cache_scope": compare_scope,
        "source_snapshot": {
            "order": row.get("order"),
            "ts": row.get("ts"),
            "logical_ts": snapshot_logical_time_us(row),
            "event_name": row.get("event_name"),
            "source_event_name": row.get("source_event_name"),
            "pid": row.get("pid"),
            "tid": row.get("tid"),
            "object_type": row.get("object_type"),
            "object_id": row.get("object_id"),
        },
        "async_classification": injection.get("async_classification", {}),
        "state_counts": {key: len(value) for key, value in state.items()},
    }
    return event, manifest_row


def build_augmented_trace(args: argparse.Namespace) -> dict[str, Any]:
    state_keys = list(dict.fromkeys(args.state_key or list(DEFAULT_STATE_KEYS)))
    report = load_json(args.divergence_report)
    injections = report.get("async_injections")
    if not isinstance(injections, list) or not injections:
        raise ValueError("divergence report contains no async_injections")

    oracle_paths = args.oracle_trace or [args.base_trace]
    rows_by_order = snapshot_rows_by_order(oracle_paths)
    injection_events: list[dict[str, Any]] = []
    manifest_rows: list[dict[str, Any]] = []
    for index, injection in enumerate(injections, start=1):
        if not isinstance(injection, dict):
            continue
        snapshot = injection.get("oracle_snapshot")
        if not isinstance(snapshot, dict):
            raise ValueError(f"async injection {index} has no oracle_snapshot")
        order = int(snapshot.get("order") or -1)
        row = rows_by_order.get(order)
        if row is None:
            raise ValueError(f"could not find oracle snapshot order {order}")
        event, manifest_row = build_injection_event(injection, row, state_keys, args.page_key_mode, index)
        injection_events.append(event)
        manifest_rows.append(manifest_row)

    base_payload = load_json(args.base_trace)
    events = trace_events(base_payload)
    combined_events = events + injection_events
    combined_events.sort(key=lambda event: (int(float(event.get("ts") or 0)), str(event.get("name") or "")))
    augmented_payload = dict(base_payload) if isinstance(base_payload, dict) else {}
    augmented_payload["traceEvents"] = combined_events

    args.output_dir.mkdir(parents=True, exist_ok=True)
    augmented_trace = args.output_dir / "async_elided_input_trace.json"
    write_json(augmented_trace, augmented_payload)

    config = load_json(args.base_config)
    config["input"] = {"trace_paths": [str(augmented_trace)]}
    config["output_dir"] = str(args.output_dir)
    validation = config.setdefault("validation", {})
    if isinstance(validation, dict):
        hicache_state = validation.setdefault("hicache_state", {})
        if isinstance(hicache_state, dict):
            hicache_state["oracle_trace_paths"] = [str(path) for path in oracle_paths]
    config_path = args.output_dir / "async_elided_modeling_config.json"
    write_json(config_path, config)

    manifest = {
        "schema": "trace_sim.hicache.async_elision_trace.v1",
        "base_config": str(args.base_config),
        "base_trace": str(args.base_trace),
        "oracle_traces": [str(path) for path in oracle_paths],
        "divergence_report": str(args.divergence_report),
        "augmented_trace": str(augmented_trace),
        "modeling_config": str(config_path),
        "state_keys": state_keys,
        "injection_count": len(injection_events),
        "injections": manifest_rows,
        "note": "Diagnostic only: synthetic events consume oracle_state to elide async divergences.",
    }
    manifest_path = args.output_dir / "async_elision_manifest.json"
    write_json(manifest_path, manifest)
    return manifest


def main(argv: list[str] | None = None) -> int:
    manifest = build_augmented_trace(parse_args(argv))
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
