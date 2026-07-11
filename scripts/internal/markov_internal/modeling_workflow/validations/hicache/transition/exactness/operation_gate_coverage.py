"""Coverage and readiness checks for diagnostic operation gates."""

from __future__ import annotations

import collections
from typing import Any

from .taxonomy_constants import MARKER_DELTA_KINDS


def build_transition_patch_gate_coverage(
    records: list[dict[str, Any]], operation_gates: list[dict[str, Any]], *, sample_limit: int
) -> dict[str, Any]:
    """Check whether operation gates account for every model transition."""

    covered_ordinals: set[int] = set()
    unresolved: list[dict[str, Any]] = []
    physical_with_marker_kind: list[dict[str, Any]] = []
    for gate in operation_gates:
        provenance = gate.get("provenance", {}) if isinstance(gate.get("provenance"), dict) else {}
        for ordinal in provenance.get("transition_ordinals", []):
            parsed_ordinal = _integer_or_none(ordinal)
            if parsed_ordinal is not None:
                covered_ordinals.add(parsed_ordinal)
        if gate.get("classification") == "unresolved":
            unresolved.append(
                {"gate_id": gate.get("gate_id"), "operation_kind": gate.get("operation_kind"), "provenance": provenance}
            )
        if gate.get("classification") == "physical_candidate":
            marker_kinds = sorted(set(provenance.get("transition_kinds", [])) & MARKER_DELTA_KINDS)
            if marker_kinds:
                physical_with_marker_kind.append(
                    {
                        "gate_id": gate.get("gate_id"),
                        "operation_kind": gate.get("operation_kind"),
                        "marker_transition_kinds": marker_kinds,
                    }
                )
    missing = [
        {
            "ordinal": ordinal,
            "transition_kind": record.get("transition_kind"),
            "source_event_index": record.get("source_event_index"),
        }
        for ordinal, record in enumerate(records)
        if ordinal not in covered_ordinals
    ]
    gate_counts_by_classification = count_gates_by_field(operation_gates, "classification")
    return {
        "schema": "trace_sim.hicache.transition_patch_gate_coverage.v1",
        "transition_count": len(records),
        "covered_transition_count": len(covered_ordinals),
        "missing_transition_count": len(missing),
        "unresolved_transition_count": len(unresolved),
        "operation_gate_count": len(operation_gates),
        "operation_gate_count_by_kind": count_gates_by_kind(operation_gates),
        "operation_gate_count_by_classification": gate_counts_by_classification,
        "physical_candidate_operation_gate_count": int(gate_counts_by_classification.get("physical_candidate", 0)),
        "state_only_operation_gate_count": int(gate_counts_by_classification.get("state_marker_only", 0)),
        "coverage_ready": not missing,
        "state_marker_filter_ready": not physical_with_marker_kind,
        "unresolved_report_ready": all(
            bool(row.get("gate_id")) and isinstance(row.get("provenance"), dict) for row in unresolved
        ),
        "missing_transitions": missing[:sample_limit],
        "unresolved_operation_gates": unresolved[:sample_limit],
        "physical_candidate_marker_violations": physical_with_marker_kind[:sample_limit],
        "notes": [
            "State-only markers must not contaminate physical_candidate operation gates.",
            "Unresolved gates are never silently dropped; samples must be reported in this file.",
        ],
    }


def _integer_or_none(value: Any) -> int | None:
    """Convert one provenance ordinal while rejecting malformed values."""

    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def operation_gate_schema_ready(operation_gates: list[dict[str, Any]]) -> bool:
    """Check the minimum schema required from every operation gate."""

    required = (
        "gate_id",
        "operation_kind",
        "gate_maturity",
        "patch_allowed",
        "operation_class",
        "classification",
        "provenance",
    )
    return bool(operation_gates) and all(
        all(key in gate for key in required)
        and gate.get("gate_maturity") == "diagnostic"
        and gate.get("patch_allowed") is False
        for gate in operation_gates
    )


def count_gates_by_field(operation_gates: list[dict[str, Any]], field: str) -> dict[str, int]:
    """Count operation gates by one field."""

    return dict(sorted(collections.Counter(str(row.get(field) or "") for row in operation_gates).items()))


def count_gates_by_kind(operation_gates: list[dict[str, Any]]) -> dict[str, int]:
    """Count operation gates by operation kind."""

    counts: collections.Counter[str] = collections.Counter(
        str(row.get("operation_kind") or "") for row in operation_gates
    )
    return dict(sorted(counts.items()))


def summarize_gate_rows_by_key(rows: list[dict[str, Any]], key: str) -> dict[str, Any]:
    """Aggregate patch-gate readiness by a prediction field."""

    result: dict[str, Any] = {}
    for value in sorted({str(row.get(key) or "") for row in rows}):
        selected = [row for row in rows if str(row.get(key) or "") == value]
        result[value] = {
            "prediction_count": len(selected),
            "operation_gate_schema_ready_count": sum(1 for row in selected if row.get("operation_gate_schema_ready")),
            "transition_coverage_ready_count": sum(1 for row in selected if row.get("transition_coverage_ready")),
            "state_marker_filter_ready_count": sum(1 for row in selected if row.get("state_marker_filter_ready")),
            "unresolved_report_ready_count": sum(1 for row in selected if row.get("unresolved_report_ready")),
            "model_operation_gate_count": sum(int(row.get("model_operation_gate_count") or 0) for row in selected),
            "observed_operation_gate_count": sum(
                int(row.get("observed_operation_gate_count") or 0) for row in selected
            ),
            "unresolved_transition_count": sum(int(row.get("unresolved_transition_count") or 0) for row in selected),
        }
    return result
