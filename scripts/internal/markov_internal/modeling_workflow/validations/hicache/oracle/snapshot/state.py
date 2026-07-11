"""Extraction and final-state projection of HiCache oracle snapshots."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from markov_internal.common.trace import load_chrome_trace_events
from ...core.facts import parse_fact_or_none


def optional_float(value: Any) -> float | None:
    """Parse a numeric candidate while rejecting booleans and invalid values."""

    if value is None or isinstance(value, bool):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def extract_hicache_state_snapshots(trace_paths: list[Path]) -> list[dict[str, Any]]:
    """Extract validation-only HiCache state snapshots from oracle traces."""

    snapshots: list[dict[str, Any]] = []
    for path in trace_paths:
        if not path.is_file():
            continue
        events, _status = load_chrome_trace_events(path, auto_repair=True)
        for event in events:
            args = event.get("args") if isinstance(event.get("args"), dict) else {}
            if not isinstance(args, dict):
                continue
            fact = parse_fact_or_none(args)
            if fact is None or fact.fact_class != "oracle_state" or fact.role != "state_snapshot":
                continue
            snapshot = args.get("state_snapshot")
            if isinstance(snapshot, dict):
                # C++ aggregates processes in one DagGraph. Preserve process
                # and object identity here so final-state projection can union
                # the corresponding latest snapshots without overwriting them.
                snapshots.append(
                    {
                        "order": len(snapshots),
                        "trace_path": str(path),
                        "pid": event.get("pid"),
                        "tid": event.get("tid"),
                        "event_name": event.get("name"),
                        "source_event_name": args.get("source_event_name"),
                        "target_id": args.get("target_id"),
                        "request_id": args.get("request_id"),
                        "operation_id": args.get("operation_id"),
                        "ts": event.get("ts"),
                        "dur": event.get("dur"),
                        "object_id": snapshot.get("object_id"),
                        "state_snapshot": snapshot,
                    }
                )
    return snapshots


def latest_derived_state(snapshots: list[dict[str, Any]]) -> dict[str, Any]:
    """Union the latest completed snapshot for every process/cache object."""

    latest_by_object: dict[tuple[str, str, str], dict[str, Any]] = {}
    completed_snapshots = [row for row in snapshots if snapshot_is_completed_state(row)]
    source_snapshots = completed_snapshots if completed_snapshots else snapshots
    for row in source_snapshots:
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
            continue
        if not snapshot_is_hiradix_cache_state(row, snapshot):
            continue
        derived = derived_hicache_state_from_snapshot(snapshot)
        if isinstance(derived, dict) and any(isinstance(derived.get(key), list) for key in derived):
            object_id = str(row.get("object_id") or snapshot.get("object_id") or "")
            key = (str(row.get("trace_path") or ""), str(row.get("pid") or ""), object_id)
            current = latest_by_object.get(key)
            if current is None or snapshot_sort_key(row) >= snapshot_sort_key(current):
                latest_by_object[key] = row

    states: list[dict[str, Any]] = []
    for row in latest_by_object.values():
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict):
            continue
        if not snapshot_is_hiradix_cache_state(row, snapshot):
            continue
        derived = derived_hicache_state_from_snapshot(snapshot)
        if isinstance(derived, dict):
            states.append(derived)
    return union_hicache_states(states)


def derived_hicache_state_from_snapshot(snapshot: dict[str, Any]) -> dict[str, Any]:
    """Derive canonical state sets from raw snapshot nodes."""

    nodes = snapshot.get("nodes")
    if not isinstance(nodes, list):
        return {}

    result: dict[str, set[str]] = {
        "l1_resident_pages": set(),
        "l2_resident_pages": set(),
        "dirty_pages": set(),
        "backuped_pages": set(),
        "evicted_pages": set(),
        "locked_pages": set(),
    }
    for node in nodes:
        if not isinstance(node, dict):
            continue
        pages = page_keys_from_snapshot_hash(node.get("hash_value"))
        has_device_value = bool(node.get("has_device_value"))
        has_host_value = bool(node.get("has_host_value"))
        backuped = bool(node.get("backuped")) or has_host_value
        evicted = bool(node.get("evicted"))
        if has_device_value:
            result["l1_resident_pages"].update(pages)
        if has_host_value:
            result["l2_resident_pages"].update(pages)
        # SGLang does not reliably expose a dirty bit. Under write-back, a
        # device-resident page without a host backup is the observable proxy.
        if node.get("dirty") or (has_device_value and not backuped and not evicted):
            result["dirty_pages"].update(pages)
        if backuped:
            result["backuped_pages"].update(pages)
        if evicted:
            result["evicted_pages"].update(pages)
        if (
            int(optional_float(node.get("lock_ref")) or 0) > 0
            or int(optional_float(node.get("host_ref_counter")) or 0) > 0
        ):
            result["locked_pages"].update(pages)
    return {key: sorted(value) for key, value in result.items()}


def page_keys_from_snapshot_hash(value: Any) -> list[str]:
    """Extract page keys from a snapshot ``hash_value`` field."""

    if value is None:
        return []
    if isinstance(value, list):
        return [str(item) for item in value if item is not None]
    return [str(value)]


def normalize_hicache_state_for_oracle_compare(state: dict[str, Any], page_key_mode: str) -> dict[str, Any]:
    """Normalize every set-valued state field under the page-key mode."""

    normalized: dict[str, Any] = {}
    for key, value in state.items():
        if isinstance(value, list):
            normalized[key] = sorted(
                {normalize_hicache_page_key(item, page_key_mode) for item in value if item is not None}
            )
        else:
            normalized[key] = value
    return normalized


def normalize_hicache_page_key(value: Any, page_key_mode: str) -> str:
    """Normalize one page key, optionally removing its scope prefix."""

    page = str(value)
    if page_key_mode == "strip_scope" and "|" in page:
        return page.split("|", 1)[1]
    return page


def snapshot_sort_key(row: dict[str, Any]) -> tuple[int, int, int]:
    """Return the stable logical order of a state snapshot.

    Probe start/end rows may share a timestamp and appear in reverse file
    order. At equal timestamps, the completed end snapshot wins because it is
    the closer representation of post-call state.
    """

    ts = int(optional_float(row.get("ts")) or 0)
    source_name = str(row.get("source_event_name") or row.get("event_name") or "")
    phase_score = 1 if source_name.endswith("_end") else 0
    order = int(row.get("order") or 0)
    return (ts, phase_score, order)


def diff_hicache_sets(model_final: dict[str, Any], oracle_final: dict[str, Any]) -> dict[str, Any]:
    """Compare model and oracle final-state sets visible to both sides."""

    keys = [
        "l1_resident_pages",
        "l2_resident_pages",
        "l3_resident_pages",
        "dirty_pages",
        "backuped_pages",
        "evicted_pages",
        "locked_pages",
        "prefetch_planned_pages",
        "prefetch_ready_pages",
        "prefetch_late_pages",
        "prefetch_suppressed_pages",
    ]
    diff: dict[str, Any] = {}
    for key in keys:
        if key not in oracle_final:
            continue
        model_set = {str(item) for item in model_final.get(key, []) if item is not None}
        oracle_set = {str(item) for item in oracle_final.get(key, []) if item is not None}
        missing = sorted(oracle_set - model_set)
        extra = sorted(model_set - oracle_set)
        diff[key] = {
            "match": not missing and not extra,
            "missing_in_model": missing,
            "extra_in_model": extra,
            "model_count": len(model_set),
            "oracle_count": len(oracle_set),
        }
    return diff


def final_state_counts(state: dict[str, Any]) -> dict[str, int]:
    """Count all set-valued final-state fields, including unchecked fields."""

    counts: dict[str, int] = {}
    for key, value in sorted(state.items()):
        if isinstance(value, list):
            counts[key] = len({str(item) for item in value if item is not None})
    return counts


def unchecked_model_state_keys(model_final: dict[str, Any], oracle_final: dict[str, Any]) -> list[str]:
    """List non-empty model state sets absent from oracle snapshots.

    These fields cannot participate in ``final_state_match`` and remain
    explicit so future probe coverage does not silently change exactness.
    """

    keys: list[str] = []
    for key, value in sorted(model_final.items()):
        if isinstance(value, list) and key not in oracle_final and any(item is not None for item in value):
            keys.append(key)
    return keys


def union_hicache_states(states: Any) -> dict[str, list[str]]:
    """Union set-valued state from multiple cache objects."""

    union: dict[str, set[str]] = {}
    for state in states:
        if not isinstance(state, dict):
            continue
        for key, value in state.items():
            if not isinstance(value, list):
                continue
            target = union.setdefault(str(key), set())
            target.update(str(item) for item in value if item is not None)
    return {key: sorted(value) for key, value in union.items()}


def snapshot_object_id_prefix(row: dict[str, Any], snapshot: dict[str, Any]) -> str:
    """Return the class-like prefix of a snapshot object identifier."""

    object_id = str(row.get("object_id") or snapshot.get("object_id") or "")
    return object_id.split(":", 1)[0] if object_id else "unknown"


def snapshot_is_hiradix_cache_state(row: dict[str, Any], snapshot: dict[str, Any]) -> bool:
    """Return whether a snapshot represents a modeled ``HiRadixCache``."""

    return snapshot_object_id_prefix(row, snapshot) == "HiRadixCache"


def snapshot_is_completed_state(row: dict[str, Any]) -> bool:
    """Return whether a snapshot represents state after a completed call.

    Probe wrappers emit both pre-call start and post-call end snapshots. Using
    a trailing unmatched start row as final state would preserve transient
    lock/reference ownership. Final and timeline oracles therefore prefer end
    or phase-less rows; the final-state caller falls back only when no completed
    snapshot exists at all.
    """

    source_name = str(row.get("source_event_name") or row.get("event_name") or "")
    return event_phase(source_name) != "start"


def snapshot_logical_time_us(row: dict[str, Any]) -> int:
    """Return the logical timestamp used by snapshot timelines.

    An end snapshot represents the state change at the end of its duration, so
    its logical timestamp is ``ts + dur``.
    """

    ts = int(optional_float(row.get("ts")) or 0)
    source_name = str(row.get("source_event_name") or row.get("event_name") or "")
    if event_phase(source_name) == "end":
        ts += int(optional_float(row.get("dur")) or 0)
    return ts


def snapshot_timeline_sort_key(row: dict[str, Any]) -> tuple[int, int, int]:
    """Build a stable timeline key that orders start before end on ties."""

    source_name = str(row.get("source_event_name") or row.get("event_name") or "")
    phase_score = 0 if event_phase(source_name) == "start" else 1
    return (snapshot_logical_time_us(row), phase_score, int(row.get("order") or 0))


def event_phase(name: str) -> str:
    """Parse a start/end phase from a probe event-name suffix."""

    clean = name.split(":", 1)[0]
    if clean.endswith("_start"):
        return "start"
    if clean.endswith("_end"):
        return "end"
    return ""


def event_base_name(name: str) -> str:
    """Remove phase suffixes to obtain the shared event base name."""

    clean = name.split(":", 1)[0]
    if clean.endswith("_start"):
        return clean[: -len("_start")]
    if clean.endswith("_end"):
        return clean[: -len("_end")]
    return clean
