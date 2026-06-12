#!/usr/bin/env python3
"""Find timeline divergences between HiCache model and oracle state.

This helper is diagnostic only. It reads predicted model transitions and
oracle state snapshots, then compares normalized state sets at each completed
oracle snapshot. The optional injection mode is still diagnostic only: it
aligns the Python replay state to oracle at an async or input-boundary-looking
divergence so the scan can continue, but it never changes the model or
prediction output.
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
    snapshot_is_completed_state,
    snapshot_logical_time_us,
    snapshot_timeline_sort_key,
    union_hicache_states,
)


DEFAULT_STATE_KEYS = (
    "l1_resident_pages",
    "l2_resident_pages",
    "dirty_pages",
    "backuped_pages",
    "evicted_pages",
    "locked_pages",
)

ASYNC_NAME_KEYWORDS = (
    "async",
    "prefetch",
    "writeback",
    "write_back",
    "loadback",
    "load_back",
    "storage",
    "release",
    "maintenance",
)

ASYNC_EVIDENCE_FACT_CLASSES = {
    "source_actual",
    "timing_observation",
}

HOST_STORAGE_STATE_KEYS = {
    "l2_resident_pages",
    "backuped_pages",
    "evicted_pages",
    "pending_writeback_pages",
    "prefetch_ready_pages",
    "prefetch_late_pages",
    "prefetch_suppressed_pages",
}

LOCK_STATE_KEYS = {
    "locked_pages",
}

CAPACITY_PRESSURE_STATE_KEYS = {
    "l1_resident_pages",
    "l2_resident_pages",
    "dirty_pages",
    "backuped_pages",
    "evicted_pages",
    "pending_writeback_pages",
}

TRANSITION_TO_STATE = {
    "add_l1_resident": ("l1_resident_pages", "add"),
    "remove_l1_resident": ("l1_resident_pages", "remove"),
    "add_l2_resident": ("l2_resident_pages", "add"),
    "remove_l2_resident": ("l2_resident_pages", "remove"),
    "add_l3_resident": ("l3_resident_pages", "add"),
    "remove_l3_resident": ("l3_resident_pages", "remove"),
    "mark_dirty": ("dirty_pages", "add"),
    "clear_dirty": ("dirty_pages", "remove"),
    "mark_backuped": ("backuped_pages", "add"),
    "clear_backuped": ("backuped_pages", "remove"),
    "mark_evicted": ("evicted_pages", "add"),
    "clear_evicted": ("evicted_pages", "remove"),
    "mark_locked": ("locked_pages", "add"),
    "clear_locked": ("locked_pages", "remove"),
    "enqueue_writeback": ("pending_writeback_pages", "add"),
    "complete_writeback": ("pending_writeback_pages", "remove"),
}


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--predicted-trace", type=Path, required=True)
    parser.add_argument("--oracle-trace", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--page-key-mode", default="strip_scope", choices=("raw", "strip_scope"))
    parser.add_argument("--compare-mode", default="object", choices=("object", "union"))
    parser.add_argument(
        "--oracle-snapshot-filter",
        default="all",
        choices=("all", "model_invariant_source"),
        help="Limit oracle snapshots used for timeline comparison. Final oracle union still uses all loaded snapshots.",
    )
    parser.add_argument("--state-key", action="append", default=[])
    parser.add_argument("--sample-pages", type=int, default=12)
    parser.add_argument("--context-records", type=int, default=8)
    parser.add_argument(
        "--diagnostic-inject-async",
        action="store_true",
        help=(
            "Diagnostic mode only: when a divergence is classified as async or an input-boundary gap, "
            "replace the replay state for the compared scope with oracle state "
            "and continue scanning for non-elided divergences."
        ),
    )
    parser.add_argument(
        "--max-divergences",
        type=int,
        default=20,
        help="Maximum divergence records to report before stopping in diagnostic mode.",
    )
    parser.add_argument(
        "--async-evidence-window-us",
        type=int,
        default=10_000_000,
        help="Time window used to attach nearby source_actual/timing_observation async evidence.",
    )
    return parser.parse_args(argv)


def optional_int(value: Any, default: int = 0) -> int:
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return default


def normalize_page(value: Any, page_key_mode: str) -> str:
    return normalize_hicache_page_key(value, page_key_mode)


def normalize_state(raw_state: dict[str, Any], state_keys: list[str], page_key_mode: str) -> dict[str, list[str]]:
    normalized: dict[str, list[str]] = {}
    for key in state_keys:
        value = raw_state.get(key, set())
        if isinstance(value, set):
            items = value
        elif isinstance(value, list):
            items = {str(item) for item in value if item is not None}
        else:
            items = set()
        normalized[key] = sorted({normalize_page(item, page_key_mode) for item in items if item is not None})
    return normalized


def diff_states(model: dict[str, list[str]], oracle: dict[str, list[str]], sample_pages: int) -> dict[str, Any]:
    diff: dict[str, Any] = {}
    for key in sorted(set(model) | set(oracle)):
        model_set = set(model.get(key, []))
        oracle_set = set(oracle.get(key, []))
        missing = sorted(oracle_set - model_set)
        extra = sorted(model_set - oracle_set)
        if not missing and not extra:
            continue
        diff[key] = {
            "model_count": len(model_set),
            "oracle_count": len(oracle_set),
            "missing_in_model_count": len(missing),
            "extra_in_model_count": len(extra),
            "missing_in_model_sample": missing[:sample_pages],
            "extra_in_model_sample": extra[:sample_pages],
        }
    return diff


def state_counts(state: dict[str, list[str]]) -> dict[str, int]:
    return {key: len(set(value)) for key, value in sorted(state.items())}


def simplify_record(record: dict[str, Any], page_key_mode: str, sample_pages: int) -> dict[str, Any]:
    pages = record.get("target_page_set")
    if not isinstance(pages, list):
        pages = []
    normalized_pages = [normalize_page(page, page_key_mode) for page in pages if page is not None]
    return {
        "source_event_index": record.get("source_event_index"),
        "source_event_name": record.get("source_event_name"),
        "event_base_name": record.get("event_base_name"),
        "transition_kind": record.get("transition_kind"),
        "ts": record.get("ts"),
        "request_id": record.get("request_id"),
        "operation_id": record.get("operation_id"),
        "cache_scope": record.get("cache_scope"),
        "page_count": len(normalized_pages),
        "pages_sample": normalized_pages[:sample_pages],
    }


def simplify_snapshot(row: dict[str, Any]) -> dict[str, Any]:
    return {
        "order": row.get("order"),
        "ts": row.get("ts"),
        "logical_ts": snapshot_logical_time_us(row),
        "event_name": row.get("event_name"),
        "source_event_name": row.get("source_event_name"),
        "target_id": row.get("target_id"),
        "request_id": row.get("request_id"),
        "operation_id": row.get("operation_id"),
        "pid": row.get("pid"),
        "tid": row.get("tid"),
        "object_type": row.get("object_type"),
        "object_id": row.get("object_id"),
    }


def record_sort_key(item: tuple[int, dict[str, Any]]) -> tuple[int, int, int]:
    index, record = item
    return (
        optional_int(record.get("ts")),
        optional_int(record.get("source_event_index")),
        index,
    )


def load_predicted_records(path: Path) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    payload = load_json(path)
    records = payload.get("records") if isinstance(payload, dict) else []
    if not isinstance(records, list):
        records = []
    typed_records = [record for record in records if isinstance(record, dict)]
    final_state = payload.get("final_state") if isinstance(payload.get("final_state"), dict) else {}
    return typed_records, final_state


def truthy(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return value.strip().lower() in {"1", "true", "yes", "y"}
    return False


def model_invariant_event_keys(paths: list[Path]) -> set[tuple[str, str, str, str, str]]:
    keys: set[tuple[str, str, str, str, str]] = set()
    for path in paths:
        if not path.is_file():
            continue
        try:
            payload = load_json(path)
        except json.JSONDecodeError:
            continue
        events = payload.get("traceEvents") if isinstance(payload, dict) else payload
        if not isinstance(events, list):
            continue
        for event in events:
            if not isinstance(event, dict):
                continue
            args = event.get("args") if isinstance(event.get("args"), dict) else {}
            if not truthy(args.get("model_input", True)):
                continue
            if str(args.get("fact_class") or "") != "invariant_state":
                continue
            keys.add(
                (
                    str(path),
                    str(event.get("pid") or ""),
                    str(event.get("tid") or ""),
                    str(event.get("name") or ""),
                    str(event.get("ts") or ""),
                )
            )
    return keys


def oracle_row_source_key(row: dict[str, Any]) -> tuple[str, str, str, str, str]:
    return (
        str(row.get("trace_path") or ""),
        str(row.get("pid") or ""),
        str(row.get("tid") or ""),
        str(row.get("source_event_name") or ""),
        str(row.get("ts") or ""),
    )


def load_oracle_rows(paths: list[Path], snapshot_filter: str = "all") -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    model_invariant_keys = model_invariant_event_keys(paths) if snapshot_filter == "model_invariant_source" else set()
    for row in extract_hicache_state_snapshots(paths):
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
            continue
        object_type = str(row.get("object_type") or snapshot.get("object_type") or "")
        if "RadixCache" not in object_type:
            continue
        if not str(row.get("object_id") or snapshot.get("object_id") or ""):
            continue
        if not snapshot_is_completed_state(row):
            continue
        if snapshot_filter == "model_invariant_source" and oracle_row_source_key(row) not in model_invariant_keys:
            continue
        rows.append(row)
    return sorted(rows, key=snapshot_timeline_sort_key)


def empty_raw_state(state_keys: list[str]) -> dict[str, set[str]]:
    return {key: set() for key in state_keys}


def union_raw_states(states: Any, state_keys: list[str], page_key_mode: str) -> dict[str, set[str]]:
    result = empty_raw_state(state_keys)
    for state in states:
        if not isinstance(state, dict):
            continue
        for key in state_keys:
            value = state.get(key)
            if isinstance(value, set):
                result[key].update(normalize_page(item, page_key_mode) for item in value if item is not None)
            elif isinstance(value, list):
                result[key].update(normalize_page(item, page_key_mode) for item in value if item is not None)
    return result


def apply_model_record(record: dict[str, Any], raw_state: dict[str, set[str]], page_key_mode: str) -> None:
    kind = str(record.get("transition_kind") or "")
    mapping = TRANSITION_TO_STATE.get(kind)
    if mapping is None:
        return
    state_key, action = mapping
    pages = record.get("target_page_set")
    if not isinstance(pages, list):
        return
    target = raw_state.setdefault(state_key, set())
    for page in pages:
        if page is None:
            continue
        text = normalize_page(page, page_key_mode)
        if action == "add":
            target.add(text)
            if kind == "add_l1_resident":
                raw_state.setdefault("evicted_pages", set()).discard(text)
        else:
            target.discard(text)


def event_text(row: dict[str, Any]) -> str:
    values = (
        row.get("event_name"),
        row.get("source_event_name"),
        row.get("target_id"),
        row.get("request_id"),
        row.get("operation_id"),
    )
    return " ".join(str(value or "").lower() for value in values)


def has_async_keyword(text: str) -> bool:
    return any(keyword in text for keyword in ASYNC_NAME_KEYWORDS)


def diff_direction_counts(diff: dict[str, Any]) -> dict[str, int]:
    missing = 0
    extra = 0
    for row in diff.values():
        if not isinstance(row, dict):
            continue
        missing += optional_int(row.get("missing_in_model_count"))
        extra += optional_int(row.get("extra_in_model_count"))
    return {"missing_in_model": missing, "extra_in_model": extra}


def affected_state_keys(diff: dict[str, Any]) -> set[str]:
    return {str(key) for key, value in diff.items() if isinstance(value, dict)}


def load_trace_events(paths: list[Path]) -> dict[str, list[dict[str, Any]]]:
    events_by_path: dict[str, list[dict[str, Any]]] = {}
    for path in paths:
        if not path.is_file():
            continue
        try:
            payload = load_json(path)
        except json.JSONDecodeError:
            continue
        events = payload.get("traceEvents") if isinstance(payload, dict) else payload
        if not isinstance(events, list):
            continue
        rows = [event for event in events if isinstance(event, dict)]
        events_by_path[str(path)] = rows
    return events_by_path


def simplify_evidence_event(event: dict[str, Any]) -> dict[str, Any]:
    args = event.get("args") if isinstance(event.get("args"), dict) else {}
    fields = (
        "fact_class",
        "event_role",
        "fact_granularity",
        "check_kind",
        "request_id",
        "operation_id",
        "completed_tokens",
        "ready_pages_estimate",
        "target_id",
    )
    return {
        "ts": event.get("ts"),
        "name": event.get("name"),
        "pid": event.get("pid"),
        "tid": event.get("tid"),
        "args": {key: args.get(key) for key in fields if key in args},
    }


def evidence_text(events: list[dict[str, Any]]) -> str:
    pieces: list[str] = []
    for event in events:
        pieces.append(str(event.get("name") or ""))
        args = event.get("args") if isinstance(event.get("args"), dict) else {}
        for key in ("event_role", "target_id", "check_kind", "diagnostic_kind"):
            pieces.append(str(args.get(key) or ""))
    return " ".join(piece.lower() for piece in pieces if piece)


def nearby_async_evidence(
    row: dict[str, Any],
    events_by_path: dict[str, list[dict[str, Any]]],
    window_us: int,
    sample_pages: int,
) -> list[dict[str, Any]]:
    trace_path = str(row.get("trace_path") or "")
    events = events_by_path.get(trace_path, [])
    if not events:
        return []
    ts = optional_int(row.get("ts"))
    pid = str(row.get("pid") or "")
    evidence: list[dict[str, Any]] = []
    for event in events:
        event_ts = optional_int(event.get("ts"))
        if abs(event_ts - ts) > window_us:
            continue
        if pid and str(event.get("pid") or "") != pid:
            continue
        args = event.get("args") if isinstance(event.get("args"), dict) else {}
        fact_class = str(args.get("fact_class") or "")
        name_text = str(event.get("name") or "").lower()
        target_text = str(args.get("target_id") or "").lower()
        if fact_class not in ASYNC_EVIDENCE_FACT_CLASSES and not has_async_keyword(name_text + " " + target_text):
            continue
        if fact_class not in ASYNC_EVIDENCE_FACT_CLASSES and len(evidence) >= sample_pages:
            continue
        evidence.append(simplify_evidence_event(event))
        if len(evidence) >= max(sample_pages, 1) * 4:
            break
    return evidence


def boundary_classification(
    *,
    classification: str,
    confidence: str,
    reason: str,
    is_async: bool,
    diagnostic_elision_allowed: bool,
    directions: dict[str, int],
    affected: set[str],
    evidence: list[dict[str, Any]],
) -> dict[str, Any]:
    return {
        "is_async": is_async,
        "is_diagnostic_boundary": diagnostic_elision_allowed,
        "diagnostic_elision_allowed": diagnostic_elision_allowed,
        "classification": classification,
        "confidence": confidence,
        "reason": reason,
        "direction_counts": directions,
        "affected_state_keys": sorted(affected),
        "nearby_async_evidence_count": len(evidence),
    }


def classify_async_divergence(diff: dict[str, Any], row: dict[str, Any], evidence: list[dict[str, Any]]) -> dict[str, Any]:
    directions = diff_direction_counts(diff)
    affected = affected_state_keys(diff)
    text = event_text(row)
    nearby_text = evidence_text(evidence)
    evidence_fact_classes = {
        str((event.get("args") if isinstance(event.get("args"), dict) else {}).get("fact_class") or "")
        for event in evidence
    }
    has_source_async_evidence = bool(evidence_fact_classes.intersection(ASYNC_EVIDENCE_FACT_CLASSES)) and has_async_keyword(nearby_text)

    if affected.issubset(LOCK_STATE_KEYS) and ("lock" in text or "lock" in nearby_text):
        return boundary_classification(
            classification="lock_ref_transient_boundary",
            confidence="high",
            reason="lock/ref snapshots are source_actual transient boundaries; normal invariant input does not model lock deltas",
            is_async=False,
            diagnostic_elision_allowed=True,
            directions=directions,
            affected=affected,
            evidence=evidence,
        )

    if affected.issubset(CAPACITY_PRESSURE_STATE_KEYS) and ("capacity" in text or "capacity" in nearby_text):
        return boundary_classification(
            classification="target_capacity_pressure_boundary",
            confidence="high",
            reason="device capacity pressure is target/control-flow derived and is not represented by a normal atomic invariant event",
            is_async=False,
            diagnostic_elision_allowed=True,
            directions=directions,
            affected=affected,
            evidence=evidence,
        )

    if affected.issubset(CAPACITY_PRESSURE_STATE_KEYS) and ("lock" in text or "lock" in nearby_text):
        return boundary_classification(
            classification="lock_protected_capacity_boundary",
            confidence="medium",
            reason="lock/ref source boundaries can change capacity victim eligibility, but normal invariant input does not model lock deltas",
            is_async=False,
            diagnostic_elision_allowed=True,
            directions=directions,
            affected=affected,
            evidence=evidence,
        )

    if "prefetch" in text and directions["missing_in_model"] > 0 and directions["extra_in_model"] == 0:
        if affected.issubset(HOST_STORAGE_STATE_KEYS):
            return boundary_classification(
                classification="async_prefetch_storage_completion",
                confidence="high",
                reason="prefetch checkpoint made host/storage pages visible in oracle, but completed page list is not invariant input",
                is_async=True,
                diagnostic_elision_allowed=True,
                directions=directions,
                affected=affected,
                evidence=evidence,
            )

    if has_async_keyword(text) and has_source_async_evidence:
        return boundary_classification(
            classification="async_checkpoint_with_source_progress_evidence",
            confidence="medium",
            reason="divergence occurred at an async lifecycle/checkpoint event with nearby source_actual or timing_observation evidence",
            is_async=True,
            diagnostic_elision_allowed=True,
            directions=directions,
            affected=affected,
            evidence=evidence,
        )

    if has_async_keyword(text) and affected.issubset(HOST_STORAGE_STATE_KEYS) and (
        directions["missing_in_model"] == 0 or directions["extra_in_model"] == 0
    ):
        return boundary_classification(
            classification="async_host_storage_lifecycle_candidate",
            confidence="medium",
            reason="host/storage-only one-direction diff at an async-looking lifecycle event",
            is_async=True,
            diagnostic_elision_allowed=True,
            directions=directions,
            affected=affected,
            evidence=evidence,
        )

    if affected.issubset(HOST_STORAGE_STATE_KEYS) and any(
        keyword in text or keyword in nearby_text for keyword in ("maintenance", "writeback", "write_back", "storage", "release", "host")
    ):
        return boundary_classification(
            classification="host_storage_visibility_boundary",
            confidence="medium",
            reason="host/storage visibility changed at a source lifecycle or maintenance boundary that normal invariant input does not encode",
            is_async=False,
            diagnostic_elision_allowed=True,
            directions=directions,
            affected=affected,
            evidence=evidence,
        )

    return boundary_classification(
        classification="unresolved_or_non_async",
        confidence="none",
        reason="divergence does not match conservative async/input-boundary elision patterns",
        is_async=False,
        diagnostic_elision_allowed=False,
        directions=directions,
        affected=affected,
        evidence=evidence,
    )


def inject_oracle_state_for_scope(
    model_states_by_scope: dict[str, dict[str, set[str]]],
    compare_scope: str,
    oracle_state: dict[str, list[str]],
    state_keys: list[str],
) -> dict[str, Any]:
    raw_state = model_states_by_scope.setdefault(compare_scope, empty_raw_state(state_keys))
    changes: dict[str, Any] = {}
    for key in state_keys:
        before = set(raw_state.get(key, set()))
        after = set(str(page) for page in oracle_state.get(key, []) if page is not None)
        raw_state[key] = set(after)
        added = sorted(after - before)
        removed = sorted(before - after)
        if added or removed:
            changes[key] = {
                "before_count": len(before),
                "after_count": len(after),
                "added_count": len(added),
                "removed_count": len(removed),
                "added_sample": added[:12],
                "removed_sample": removed[:12],
            }
    return {
        "compare_scope": compare_scope,
        "mode": "replace_compared_scope_state_with_oracle",
        "changed_state_keys": sorted(changes),
        "changes": changes,
    }


def oracle_scope_for_row(row: dict[str, Any], known_scopes: set[str]) -> str:
    object_id = str(row.get("object_id") or "")
    if object_id:
        for scope in sorted(known_scopes):
            if object_id in scope:
                return scope
        return object_id
    pid = row.get("pid")
    return str(pid) if pid is not None else ""


def comparison_states(
    compare_mode: str,
    row: dict[str, Any],
    state_keys: list[str],
    model_states_by_scope: dict[str, dict[str, set[str]]],
    oracle_object_states: dict[tuple[str, str, str], dict[str, Any]],
    page_key_mode: str,
    known_scopes: set[str],
) -> tuple[str, dict[str, list[str]], dict[str, list[str]]]:
    if compare_mode == "union":
        model_raw = union_raw_states(model_states_by_scope.values(), state_keys, page_key_mode)
        oracle_raw = union_hicache_states(oracle_object_states.values())
        return "union", normalize_state(model_raw, state_keys, page_key_mode), normalize_state(oracle_raw, state_keys, page_key_mode)

    scope = oracle_scope_for_row(row, known_scopes)
    model_raw = model_states_by_scope.setdefault(scope, empty_raw_state(state_keys))
    oracle_key = (str(row.get("trace_path") or ""), str(row.get("pid") or ""), str(row.get("object_id") or ""))
    oracle_raw = oracle_object_states.get(oracle_key, {})
    return scope, normalize_state(model_raw, state_keys, page_key_mode), normalize_state(oracle_raw, state_keys, page_key_mode)


def final_oracle_union(rows: list[dict[str, Any]]) -> dict[str, Any]:
    object_states: dict[tuple[str, str, str], dict[str, Any]] = {}
    for row in rows:
        key = (str(row.get("trace_path") or ""), str(row.get("pid") or ""), str(row.get("object_id") or ""))
        object_states[key] = derived_hicache_state_from_snapshot(row.get("state_snapshot", {}))
    return union_hicache_states(object_states.values())


def build_report(args: argparse.Namespace) -> dict[str, Any]:
    state_keys = list(dict.fromkeys(args.state_key or list(DEFAULT_STATE_KEYS)))
    records, trace_final_state = load_predicted_records(args.predicted_trace)
    indexed_records = sorted(enumerate(records), key=record_sort_key)
    oracle_rows = load_oracle_rows(args.oracle_trace, args.oracle_snapshot_filter)
    oracle_final_rows = load_oracle_rows(args.oracle_trace, "all")
    trace_events = load_trace_events(args.oracle_trace) if args.diagnostic_inject_async else {}

    known_scopes = {str(record.get("cache_scope") or "") for record in records if str(record.get("cache_scope") or "")}
    model_states_by_scope: dict[str, dict[str, set[str]]] = {}
    object_states: dict[tuple[str, str, str], dict[str, Any]] = {}
    cursor = 0
    first_divergence: dict[str, Any] | None = None
    first_non_async_divergence: dict[str, Any] | None = None
    last_matched: dict[str, Any] | None = None
    divergences: list[dict[str, Any]] = []
    async_injections: list[dict[str, Any]] = []

    for step_index, row in enumerate(oracle_rows):
        cutoff_ts = snapshot_logical_time_us(row)
        while cursor < len(indexed_records) and optional_int(indexed_records[cursor][1].get("ts")) <= cutoff_ts:
            record = indexed_records[cursor][1]
            scope = str(record.get("cache_scope") or "")
            apply_model_record(record, model_states_by_scope.setdefault(scope, empty_raw_state(state_keys)), args.page_key_mode)
            cursor += 1

        key = (str(row.get("trace_path") or ""), str(row.get("pid") or ""), str(row.get("object_id") or ""))
        object_states[key] = derived_hicache_state_from_snapshot(row.get("state_snapshot", {}))
        compare_scope, model_state, oracle_state = comparison_states(
            args.compare_mode,
            row,
            state_keys,
            model_states_by_scope,
            object_states,
            args.page_key_mode,
            known_scopes,
        )
        diff = diff_states(model_state, oracle_state, args.sample_pages)
        if diff:
            before_start = max(0, cursor - args.context_records)
            after_end = min(len(indexed_records), cursor + args.context_records)
            nearby_records = indexed_records[before_start:after_end]
            nearby_scope_records = [item for item in nearby_records if str(item[1].get("cache_scope") or "") == compare_scope]
            evidence = nearby_async_evidence(row, trace_events, args.async_evidence_window_us, args.sample_pages) if trace_events else []
            divergence = {
                "oracle_step_index": step_index,
                "compare_mode": args.compare_mode,
                "compare_scope": compare_scope,
                "oracle_snapshot": simplify_snapshot(row),
                "model_records_applied": cursor,
                "model_total_records": len(indexed_records),
                "model_last_applied_record": simplify_record(indexed_records[cursor - 1][1], args.page_key_mode, args.sample_pages)
                if cursor > 0
                else None,
                "model_next_record": simplify_record(indexed_records[cursor][1], args.page_key_mode, args.sample_pages)
                if cursor < len(indexed_records)
                else None,
                "last_matched": last_matched,
                "diff": diff,
                "model_counts": state_counts(model_state),
                "oracle_counts": state_counts(oracle_state),
                "nearby_model_records": [
                    simplify_record(record, args.page_key_mode, args.sample_pages) for _idx, record in nearby_records
                ],
                "nearby_scope_model_records": [
                    simplify_record(record, args.page_key_mode, args.sample_pages) for _idx, record in nearby_scope_records
                ],
            }
            classification = classify_async_divergence(diff, row, evidence)
            divergence["async_classification"] = classification
            divergence["nearby_async_evidence"] = evidence[: args.sample_pages]
            if first_divergence is None:
                first_divergence = divergence

            can_inject = (
                args.diagnostic_inject_async
                and args.compare_mode == "object"
                and bool(classification.get("diagnostic_elision_allowed", classification.get("is_async")))
                and len(async_injections) < args.max_divergences
            )
            if can_inject:
                injection = inject_oracle_state_for_scope(model_states_by_scope, compare_scope, oracle_state, state_keys)
                injection.update(
                    {
                        "oracle_step_index": step_index,
                        "oracle_snapshot": simplify_snapshot(row),
                        "async_classification": classification,
                    }
                )
                divergence["diagnostic_injection"] = injection
                async_injections.append(injection)
                divergences.append(divergence)
                last_matched = {
                    "oracle_step_index": step_index,
                    "compare_mode": args.compare_mode,
                    "compare_scope": compare_scope,
                    "oracle_snapshot": simplify_snapshot(row),
                    "model_records_applied": cursor,
                    "counts": state_counts(oracle_state),
                    "matched_by_diagnostic_boundary_injection": True,
                }
                continue

            if args.diagnostic_inject_async and args.compare_mode != "object":
                divergence["diagnostic_injection_skipped_reason"] = "diagnostic injection currently supports object compare-mode only"
            elif (
                args.diagnostic_inject_async
                and bool(classification.get("diagnostic_elision_allowed", classification.get("is_async")))
                and len(async_injections) >= args.max_divergences
            ):
                divergence["diagnostic_injection_skipped_reason"] = "max divergence/injection limit reached"
            first_non_async_divergence = divergence
            divergences.append(divergence)
            break
        last_matched = {
            "oracle_step_index": step_index,
            "compare_mode": args.compare_mode,
            "compare_scope": compare_scope,
            "oracle_snapshot": simplify_snapshot(row),
            "model_records_applied": cursor,
            "counts": state_counts(model_state),
        }

    for _idx, record in indexed_records[cursor:]:
        scope = str(record.get("cache_scope") or "")
        apply_model_record(record, model_states_by_scope.setdefault(scope, empty_raw_state(state_keys)), args.page_key_mode)

    replay_final = normalize_state(union_raw_states(model_states_by_scope.values(), state_keys, args.page_key_mode), state_keys, args.page_key_mode)
    trace_final = normalize_state(trace_final_state, state_keys, args.page_key_mode)
    oracle_final = normalize_state(final_oracle_union(oracle_final_rows), state_keys, args.page_key_mode)
    replay_vs_trace_final = diff_states(replay_final, trace_final, args.sample_pages)
    replay_vs_oracle_final = diff_states(replay_final, oracle_final, args.sample_pages)

    return {
        "schema": "trace_sim.hicache.state_trace_divergence.v1",
        "predicted_trace": str(args.predicted_trace),
        "oracle_traces": [str(path) for path in args.oracle_trace],
        "page_key_mode": args.page_key_mode,
        "compare_mode": args.compare_mode,
        "oracle_snapshot_filter": args.oracle_snapshot_filter,
        "state_keys": state_keys,
        "oracle_completed_radix_snapshot_count": len(oracle_rows),
        "model_transition_count": len(indexed_records),
        "diagnostic_async_injection_enabled": bool(args.diagnostic_inject_async),
        "diagnostic_async_injection_count": len(async_injections),
        "diagnostic_boundary_injection_count": len(async_injections),
        "async_injections": async_injections,
        "diagnostic_boundary_injections": async_injections,
        "divergence_count": len(divergences),
        "divergences": divergences,
        "first_divergence": first_divergence,
        "first_non_async_divergence": first_non_async_divergence,
        "matched_until_final_oracle_snapshot": first_divergence is None,
        "matched_after_diagnostic_async_elision_until_final_oracle_snapshot": first_non_async_divergence is None,
        "matched_after_diagnostic_boundary_elision_until_final_oracle_snapshot": first_non_async_divergence is None,
        "final_counts": {
            "diagnostic_replay": state_counts(replay_final),
            "model_trace_final": state_counts(trace_final),
            "oracle_timeline_union": state_counts(oracle_final),
        },
        "model_replay_vs_trace_final_diff": replay_vs_trace_final,
        "diagnostic_replay_vs_oracle_final_diff": replay_vs_oracle_final,
    }


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    report = build_report(args)
    text = json.dumps(report, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n", encoding="utf-8")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
