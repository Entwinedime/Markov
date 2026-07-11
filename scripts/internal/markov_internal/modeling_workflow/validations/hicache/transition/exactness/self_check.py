"""Model-side consistency checks for predicted HiCache transitions."""

from __future__ import annotations

import collections
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from ...oracle.diff.multiset import count_rows_by_transition_kind
from ..replay.record_schema import (
    ADVISORY_RECORD_FIELDS,
    CORE_RECORD_FIELDS,
    KNOWN_TRANSITION_KINDS,
    load_predicted_trace,
    missing_core_field,
    optional_int,
    predicted_final_state,
    predicted_records,
    record_pages,
)
from ..replay.engine import replay_predicted_records
from ..replay.final_state_compare import compare_replay_final_state


PENDING_LIFECYCLE_ORDER = (
    "writeback",
    "storage_backup",
    "loadback",
    "write_through_backup",
)

PENDING_LIFECYCLE_TRANSITIONS = {
    "enqueue_writeback": ("writeback", 1, False),
    "complete_writeback": ("writeback", -1, False),
    "cancel_writeback": ("writeback", -1, False),
    "enqueue_storage_backup": ("storage_backup", 1, False),
    "complete_storage_backup": ("storage_backup", -1, False),
    "enqueue_loadback": ("loadback", 1, False),
    "complete_loadback": ("loadback", -1, False),
    "enqueue_write_through_backup": ("write_through_backup", 1, True),
    "complete_write_through_backup": ("write_through_backup", -1, True),
}


@dataclass
class _SchemaDiagnostics:
    """Accumulate bounded per-record schema diagnostics in trace order."""

    sample_limit: int
    missing_required: list[dict[str, Any]] = field(default_factory=list)
    missing_required_count: int = 0
    missing_advisory: collections.Counter[str] = field(default_factory=collections.Counter)
    unknown_kinds: collections.Counter[str] = field(default_factory=collections.Counter)
    empty_page_mutations: list[dict[str, Any]] = field(default_factory=list)
    empty_page_mutation_count: int = 0
    non_monotonic_indices: list[dict[str, Any]] = field(default_factory=list)
    non_monotonic_index_count: int = 0
    previous_source_index: int = -1

    def observe(self, record: dict[str, Any], ordinal: int) -> None:
        """Validate one record and retain only the configured diagnostic sample."""

        kind = str(record.get("transition_kind") or "")
        if kind not in KNOWN_TRANSITION_KINDS:
            self.unknown_kinds[kind or "<empty>"] += 1
        missing = [field_name for field_name in CORE_RECORD_FIELDS if missing_core_field(record, field_name)]
        self.missing_required_count += len(missing)
        if missing and len(self.missing_required) < self.sample_limit:
            self.missing_required.append({"ordinal": ordinal, "missing_fields": missing, "transition_kind": kind})
        for field_name in ADVISORY_RECORD_FIELDS:
            if missing_core_field(record, field_name):
                self.missing_advisory[field_name] += 1
        self._observe_page_mutation(record, ordinal, kind)
        self._observe_source_index(record, ordinal)

    def _observe_page_mutation(self, record: dict[str, Any], ordinal: int, kind: str) -> None:
        """Record non-counter transitions that carry no page identity."""

        if not kind or kind == "increment_hit_count" or record_pages(record):
            return
        self.empty_page_mutation_count += 1
        if len(self.empty_page_mutations) < self.sample_limit:
            self.empty_page_mutations.append(
                {
                    "ordinal": ordinal,
                    "transition_kind": kind,
                    "source_event_index": record.get("source_event_index"),
                }
            )

    def _observe_source_index(self, record: dict[str, Any], ordinal: int) -> None:
        """Check monotonic source-event attribution without requiring contiguity."""

        source_index = optional_int(record.get("source_event_index"), -1)
        if source_index >= 0 and self.previous_source_index > source_index:
            self.non_monotonic_index_count += 1
            if len(self.non_monotonic_indices) < self.sample_limit:
                self.non_monotonic_indices.append(
                    {
                        "ordinal": ordinal,
                        "previous_source_event_index": self.previous_source_index,
                        "source_event_index": source_index,
                    }
                )
        if source_index >= 0:
            self.previous_source_index = max(self.previous_source_index, source_index)

    def as_payload(self, predicted: dict[str, Any], records: list[dict[str, Any]]) -> dict[str, Any]:
        """Serialize accumulated diagnostics with top-level schema checks."""

        return {
            "schema_name": predicted.get("schema"),
            "top_level_ready": (
                predicted.get("schema") == "trace_sim.hicache.predicted_state_trace.v1"
                and isinstance(predicted.get("records"), list)
                and isinstance(predicted.get("final_state"), dict)
            ),
            "top_level_errors": [
                error
                for error, failed in (
                    ("unexpected_schema", predicted.get("schema") != "trace_sim.hicache.predicted_state_trace.v1"),
                    ("records_not_array", not isinstance(predicted.get("records"), list)),
                    ("final_state_not_object", not isinstance(predicted.get("final_state"), dict)),
                )
                if failed
            ],
            "record_count": len(records),
            "unknown_transition_kinds": dict(sorted(self.unknown_kinds.items())),
            "missing_required_field_count": self.missing_required_count,
            "missing_required_fields": self.missing_required,
            "missing_advisory_field_counts": dict(sorted(self.missing_advisory.items())),
            "transition_id_present": all("transition_id" in record for record in records),
            "transition_id_note": "predicted_target_cache_state_trace.v1 may omit C++ transition_id; replay uses record ordinal when absent.",
            "empty_page_mutation_count": self.empty_page_mutation_count,
            "empty_page_mutations": self.empty_page_mutations,
            "non_monotonic_source_event_index_count": self.non_monotonic_index_count,
            "non_monotonic_source_event_indices": self.non_monotonic_indices,
        }


@dataclass
class _LifecycleCounters:
    """Track prefetch and required asynchronous lifecycle balances."""

    prefetch_by_request: dict[str, collections.Counter[str]] = field(
        default_factory=lambda: collections.defaultdict(collections.Counter)
    )
    pending_by_lifecycle: dict[str, collections.Counter[str]] = field(
        default_factory=lambda: {name: collections.Counter() for name in PENDING_LIFECYCLE_ORDER}
    )

    def observe(self, record: dict[str, Any]) -> None:
        """Apply one transition to the appropriate lifecycle balance."""

        kind = str(record.get("transition_kind") or "")
        request_key = transition_request_key(record)
        if kind.startswith("prefetch_") or kind == "apply_prefetch_host_visibility":
            self.prefetch_by_request[request_key][kind] += 1

        transition = PENDING_LIFECYCLE_TRANSITIONS.get(kind)
        if transition is None:
            return
        lifecycle, delta, scope_level = transition
        key = str(record.get("cache_scope") or "") if scope_level else request_key
        pending = self.pending_by_lifecycle[lifecycle]
        if delta > 0:
            pending[key] += 1
        elif pending[key] > 0:
            pending[key] -= 1


def build_model_self_check(predicted_trace_path: Path, *, sample_limit: int) -> dict[str, Any]:
    """Build the model-side transition consistency report."""

    predicted = load_predicted_trace(predicted_trace_path)
    records = predicted_records(predicted)
    schema_check = check_predicted_trace_schema(predicted, records, sample_limit=sample_limit)
    replay = replay_predicted_records(records, sample_limit=sample_limit)
    final_state = predicted_final_state(predicted)
    replay_check = compare_replay_final_state(replay, final_state, sample_limit=sample_limit)
    provenance_check = build_provenance_check(records, sample_limit=sample_limit)
    lifecycle_check = build_lifecycle_check(records, sample_limit=sample_limit)

    ready = (
        schema_check["top_level_ready"]
        and not schema_check["unknown_transition_kinds"]
        and schema_check["missing_required_field_count"] == 0
        and replay_check["replay_final_state_match"]
        and replay["state_constraint_violation_count"] == 0
        and lifecycle_check["unclosed_required_lifecycle_count"] == 0
    )
    return {
        "schema": "trace_sim.hicache.model_transition_self_check.v1",
        "sample_limit": sample_limit,
        "ready": ready,
        "predicted_trace_path": str(predicted_trace_path),
        "record_count": len(records),
        "schema_check": schema_check,
        "replay_check": replay_check,
        "state_constraint_check": {
            "state_constraint_violation_count": replay["state_constraint_violation_count"],
            "state_constraint_violations": replay["state_constraint_violations"],
            "ref_balance_issue_count": replay["ref_balance_issue_count"],
            "ref_balance_issues": replay["ref_balance_issues"],
            "derived_noop_transition_count": replay["derived_noop_transition_count"],
            "derived_noop_transitions": replay["derived_noop_transitions"],
        },
        "provenance_check": provenance_check,
        "lifecycle_check": lifecycle_check,
        "replayed_final_state": replay["final_state"],
        "replayed_delta_summary": {
            "delta_row_count": len(replay["delta_rows"]),
            "delta_count_by_kind": count_rows_by_transition_kind(replay["delta_rows"]),
        },
    }


def check_predicted_trace_schema(
    predicted: dict[str, Any], records: list[dict[str, Any]], *, sample_limit: int
) -> dict[str, Any]:
    """Check top-level and per-record predicted-trace schema integrity."""

    diagnostics = _SchemaDiagnostics(sample_limit)
    for ordinal, record in enumerate(records):
        diagnostics.observe(record, ordinal)
    return diagnostics.as_payload(predicted, records)


def build_provenance_check(records: list[dict[str, Any]], *, sample_limit: int) -> dict[str, Any]:
    """Check whether transitions retain minimum source provenance."""

    without_source: list[dict[str, Any]] = []
    without_source_count = 0
    without_policy_hint: list[dict[str, Any]] = []
    without_policy_hint_count = 0
    by_source_event = collections.Counter()
    for ordinal, record in enumerate(records):
        source_index = record.get("source_event_index")
        source_name = str(record.get("source_event_name") or "")
        if source_index is None or not source_name:
            without_source_count += 1
            if len(without_source) < sample_limit:
                without_source.append(
                    {
                        "ordinal": ordinal,
                        "transition_kind": record.get("transition_kind"),
                        "source_event_index": source_index,
                    }
                )
        by_source_event[source_name] += 1
        if not str(record.get("decision_reason") or ""):
            without_policy_hint_count += 1
            if len(without_policy_hint) < sample_limit:
                without_policy_hint.append({"ordinal": ordinal, "transition_kind": record.get("transition_kind")})
    return {
        "transitions_without_source_fact_count": without_source_count,
        "transitions_without_source_fact": without_source,
        "transitions_without_policy_decision_hint_count": without_policy_hint_count,
        "transitions_without_policy_decision_hint": without_policy_hint,
        "transition_count_by_source_event": dict(sorted(by_source_event.items())),
    }


def build_lifecycle_check(records: list[dict[str, Any]], *, sample_limit: int) -> dict[str, Any]:
    """Check closure of coarse asynchronous lifecycles."""

    counters = _LifecycleCounters()
    for record in records:
        counters.observe(record)

    unclosed = _unclosed_prefetch_lifecycles(counters.prefetch_by_request)
    unclosed.extend(_unclosed_pending_lifecycles(counters.pending_by_lifecycle))
    return {
        "unclosed_required_lifecycle_count": len(unclosed),
        "unclosed_required_lifecycle": unclosed[:sample_limit],
        "prefetch_request_count": len(counters.prefetch_by_request),
    }


def _unclosed_prefetch_lifecycles(
    prefetch_by_request: dict[str, collections.Counter[str]],
) -> list[dict[str, Any]]:
    """Return requests whose prefetch plans lack terminal transitions."""

    unclosed: list[dict[str, Any]] = []
    for request_key, counts in sorted(prefetch_by_request.items()):
        planned = counts.get("prefetch_planned", 0)
        terminal = (
            counts.get("prefetch_ready", 0)
            + counts.get("prefetch_revoked", 0)
            + counts.get("prefetch_suppressed", 0)
            + counts.get("prefetch_terminated", 0)
            + counts.get("prefetch_timeout_incomplete", 0)
        )
        if terminal < planned:
            unclosed.append(
                {
                    "lifecycle": "prefetch",
                    "request_key": request_key,
                    "planned": planned,
                    "terminal": terminal,
                    "counts": dict(counts),
                }
            )
    return unclosed


def _unclosed_pending_lifecycles(
    pending_by_lifecycle: dict[str, collections.Counter[str]],
) -> list[dict[str, Any]]:
    """Return positive required-lifecycle balances in stable family order.

    Write-through balances use cache scope keys because a later fact can drain
    a scope-level queue without retaining the enqueue request identifier.
    """

    unclosed: list[dict[str, Any]] = []
    for lifecycle in PENDING_LIFECYCLE_ORDER:
        pending = pending_by_lifecycle[lifecycle]
        for request_key, count in sorted(pending.items()):
            if count > 0:
                unclosed.append({"lifecycle": lifecycle, "request_key": request_key, "pending_count": count})
    return unclosed


def transition_request_key(record: dict[str, Any]) -> str:
    """Build a run-local request/operation attribution key for replay."""

    return ":".join(
        [
            str(record.get("cache_scope") or ""),
            str(record.get("request_id") or ""),
            str(record.get("operation_id") or ""),
        ]
    )
