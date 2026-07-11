"""Target-side observed transition oracle for HiCache validation."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from markov_internal.common.trace import load_chrome_trace_events
from ...core.facts import parse_fact_or_none
from ...oracle.diff.event_delta import DELTA_KIND_BY_STATE_KEY
from ...oracle.diff.timeline_delta import build_oracle_timeline_deltas
from ...oracle.snapshot.state import (
    derived_hicache_state_from_snapshot,
    extract_hicache_state_snapshots,
    latest_derived_state,
    snapshot_is_completed_state,
)
from ..replay.record_schema import ACTIVE_STATE_KEYS, state_counts
from .request_identity import canonical_request_key
from .taxonomy_constants import OBSERVED_ROLE_TO_OPERATION_KIND


SNAPSHOT_VISIBLE_STATE_KEYS = tuple(DELTA_KIND_BY_STATE_KEY)
TRANSITION_COMPARABLE_STATE_KEYS = tuple(key for key in SNAPSHOT_VISIBLE_STATE_KEYS if key != "locked_pages")


def extract_target_oracle(
    trace_paths: list[Path], target_metadata: dict[str, Any], *, sample_limit: int
) -> dict[str, Any]:
    """Build the target-side observed transition oracle."""

    snapshots = extract_hicache_state_snapshots(trace_paths)
    timeline_oracle = build_oracle_timeline_deltas(snapshots, set(SNAPSHOT_VISIBLE_STATE_KEYS))
    observed_transitions = observed_transitions_from_snapshot_rows(timeline_oracle["rows"])
    observed_operations, event_status = extract_observed_operations(trace_paths)
    final_state = latest_derived_state(snapshots)
    visible_keys = sorted(timeline_visible_keys_from_snapshots(snapshots))
    unsupported_keys = sorted(set(ACTIVE_STATE_KEYS) - set(SNAPSHOT_VISIBLE_STATE_KEYS))
    ready = bool(snapshots) and (bool(observed_operations) or bool(observed_transitions))
    return {
        "schema": "trace_sim.hicache.observed_target_transition_trace.v1",
        "sample_limit": sample_limit,
        "oracle_ready": ready,
        **target_metadata,
        "oracle_trace_files": [str(path) for path in trace_paths],
        "observability_summary": {
            "state_snapshot_count": len(snapshots),
            "completed_state_snapshot_count": sum(1 for row in snapshots if snapshot_is_completed_state(row)),
            "snapshot_delta_row_count": len(timeline_oracle["rows"]),
            "observed_operation_count": len(observed_operations),
            "observed_transition_count": len(observed_transitions),
            "visible_state_keys": visible_keys,
            "unsupported_or_unobservable_state_keys": unsupported_keys,
            "trace_load_status": event_status,
            "object_group_count": timeline_oracle.get("object_group_count", 0),
            "snapshot_count_with_object_id": timeline_oracle.get("snapshot_count_with_object_id", 0),
            "snapshot_count_without_object_id": timeline_oracle.get("snapshot_count_without_object_id", 0),
        },
        "observed_transitions": observed_transitions,
        "observed_operations": observed_operations,
        "snapshot_delta_rows": timeline_oracle["rows"],
        "final_state": final_state,
        "final_state_counts": state_counts(final_state),
        "unsupported_or_unobservable_state_keys": unsupported_keys,
        "notes": [
            "snapshot_delta_rows are derived from validation-only state_snapshot timeline and are labels only.",
            "observed_operations are source_actual/timing evidence from the target run.",
            "L3 and prefetch internal sets are model-side state unless a future probe exposes them directly.",
        ],
        "samples": {
            "observed_transitions": observed_transitions[:sample_limit],
            "observed_operations": observed_operations[:sample_limit],
        },
    }


def observed_transitions_from_snapshot_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Normalize snapshot deltas to observed transition rows."""

    result: list[dict[str, Any]] = []
    for index, row in enumerate(rows):
        pages = (
            [str(page) for page in row.get("pages", []) if page is not None]
            if isinstance(row.get("pages"), list)
            else []
        )
        result.append(
            {
                "observed_transition_id": f"snapshot_delta:{index}",
                "operation_kind": "snapshot_state_delta",
                "state_delta_kind": row.get("transition_kind") or "",
                "transition_kind": row.get("transition_kind") or "",
                "pages": pages,
                "cache_scope": row.get("cache_scope") or "",
                "request_id": row.get("request_id") or "",
                "operation_id": row.get("operation_id") or "",
                "canonical_request_key": canonical_request_key(row),
                "event_base_name": row.get("event_base_name") or "",
                "source_event_name": row.get("source_event_name") or "",
                "ts": row.get("ts"),
                "evidence_class": "oracle_state_snapshot_delta",
                "evidence_event_indices": [row.get("event_key") or ""],
                "confidence": "snapshot_delta",
            }
        )
    return result


def extract_observed_operations(trace_paths: list[Path]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Extract operation/control evidence consumable by transition validation."""

    operations: list[dict[str, Any]] = []
    statuses: list[dict[str, Any]] = []
    for path in trace_paths:
        events, status = load_chrome_trace_events(path, auto_repair=True)
        statuses.append(status.to_dict())
        for ordinal, event in enumerate(events):
            args = event.get("args") if isinstance(event.get("args"), dict) else {}
            fact = parse_fact_or_none(args)
            if fact is None or not _transition_observed_fact(fact.fact_class):
                continue
            role = fact.role
            event_kind = str(args.get("event_kind") or event.get("name") or "")
            operations.append(
                {
                    "observed_operation_id": f"{path.name}:{ordinal}",
                    "operation_kind": observed_operation_kind(role, event_kind),
                    "fact_role": role,
                    "event_kind": event_kind,
                    "event_name": event.get("name") or "",
                    "fact_class": fact.fact_class,
                    "cache_scope": args.get("cache_scope") or "",
                    "request_id": args.get("request_id") or "",
                    "operation_id": args.get("operation_id") or "",
                    "canonical_request_key": canonical_request_key(args),
                    "pages": [],
                    "ts": event.get("ts"),
                    "dur": event.get("dur"),
                    "trace_path": str(path),
                    "confidence": observed_confidence(fact.fact_class),
                }
            )
    return operations, statuses


def _transition_observed_fact(fact_class: str) -> bool:
    """Return whether a fact class is target-side transition evidence."""

    return fact_class in {"source_actual", "timing_observation"}


def observed_confidence(fact_class: str) -> str:
    """Return the evidence confidence class for an observed operation."""

    if fact_class == "source_actual":
        return "source_actual"
    return "timing"


def observed_operation_kind(role: str, event_kind: str) -> str:
    """Normalize a probe role to the operation taxonomy used by patch gates."""

    if role in OBSERVED_ROLE_TO_OPERATION_KIND:
        return OBSERVED_ROLE_TO_OPERATION_KIND[role]
    operation_kind = role or event_kind or "unknown"
    if "prefetch" in role or "prefetch" in event_kind:
        operation_kind = "prefetch"
    elif "writeback" in role or "writeback" in event_kind:
        operation_kind = "write_back_flush"
    elif "write" in role or "write" in event_kind:
        operation_kind = "write_through_backup"
    elif "lock" in role or "ref" in role:
        operation_kind = "lock_ref"
    elif "capacity" in role or "evict" in role:
        operation_kind = "capacity"
    return operation_kind


def timeline_visible_keys_from_snapshots(snapshots: list[dict[str, Any]]) -> set[str]:
    """Return state keys actually exposed by target snapshots."""

    visible: set[str] = set()
    for row in snapshots:
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
            continue
        for key, value in derived_hicache_state_from_snapshot(snapshot).items():
            if isinstance(value, list):
                visible.add(str(key))
    return visible
