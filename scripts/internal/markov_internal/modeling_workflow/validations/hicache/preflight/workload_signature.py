"""HiCache workload-identity signatures and sequence diagnostics."""

from __future__ import annotations

import collections
from pathlib import Path
from typing import Any

from markov_internal.common.digests import sha256_json
from ....types import ProfileRunRef
from ..input_contract.signature.extractor import extract_audit_events
from ..input_contract.signature.path_contract import workload_identity_path_contract
from ..input_contract.signature.primitives import trace_events
from ..input_contract.signature.types import AuditEvent


WORKLOAD_SIGNATURE_ROLES = (
    "cache_lookup_input",
    "cache_extend_input",
    "cache_lifecycle_commit",
    "prefetch_candidate_anchor",
)


class WorkloadSignatureBuilder:
    """Build the multiset gate and optional sequence diagnostics."""

    def __init__(
        self,
        run: ProfileRunRef,
        *,
        include_sequence_diagnostics: bool = False,
        event_rows: list[tuple[Path, dict[str, Any]]] | None = None,
    ) -> None:
        """Create a builder, optionally reusing already decoded trace rows."""

        self.run = run
        self.include_sequence_diagnostics = include_sequence_diagnostics
        self.roles = set(WORKLOAD_SIGNATURE_ROLES)
        self.event_rows = event_rows

    def build(self) -> dict[str, Any]:
        """Build the canonical workload signature for this profile run."""

        event_rows = self.event_rows
        if event_rows is None:
            event_rows = trace_events(list(self.run.python_probe_files))
        events, unknown_roles, unmapped_requests = extract_audit_events(
            list(self.run.python_probe_files),
            self.run.run_id,
            self.roles,
            event_rows=event_rows,
        )
        path_contract = workload_identity_path_contract(
            list(self.run.python_probe_files),
            self.roles,
            sample=0,
            event_rows=event_rows,
        )
        by_role: dict[str, collections.Counter[str]] = {role: collections.Counter() for role in self.roles}
        for event in events:
            by_role.setdefault(event.role, collections.Counter())[event.signature] += 1
        payload = {
            "roles": {
                role: [{"signature": signature, "count": count} for signature, count in sorted(counter.items())]
                for role, counter in sorted(by_role.items())
            }
        }
        signature = sha256_json(payload)
        sequence_payload = workload_sequence_diagnostic_payload(events)
        unknown = dict(sorted(unknown_roles.items()))
        unmapped = dict(sorted(unmapped_requests.items()))
        result = {
            "signature": signature,
            "ready": bool(events) and not unknown and not unmapped and bool(path_contract.get("ready")),
            "sequence_diagnostic_signature": sha256_json(sequence_payload),
            "sequence_diagnostic_ready": bool(events) and not unknown and not unmapped,
            "request_event_count": len(events),
            "roles": sorted(self.roles),
            "role_counts": {role: sum(counter.values()) for role, counter in sorted(by_role.items())},
            "unknown_workload_identity_roles": unknown,
            "unmapped_request_id_events": unmapped,
            "workload_identity_path_contract": path_contract,
        }
        if self.include_sequence_diagnostics:
            result["sequence_diagnostic_events"] = workload_sequence_diagnostic_events(events)
        return result


def workload_sequence_diagnostic_payload(events: list[AuditEvent]) -> list[dict[str, str]]:
    """Build the minimal sequence payload used for diagnostic hashing."""

    return [
        {
            "role": str(event.role),
            "signature": str(event.signature),
        }
        for event in events
    ]


def workload_sequence_diagnostic_events(events: list[AuditEvent]) -> list[dict[str, Any]]:
    """Build readable sequence rows used to explain a mismatch."""

    return [
        {
            "ordinal": event.ordinal,
            "role": event.role,
            "signature": event.signature,
            "target_id": event.target_id,
            "event_name": event.event_name,
            "seq_no": event.seq_no,
            "request_id": event.request_id,
            "request_fingerprint": event.request_fingerprint,
            "cache_scope": event.cache_scope,
        }
        for event in events
    ]


def summarize_workload_sequence_input_group(
    input_rows: list[dict[str, Any]],
    *,
    include_details: bool,
    sample_limit: int = 8,
) -> dict[str, Any]:
    """Summarize cross-config sequence diagnostics for one input."""

    sequence_signatures = sorted(
        {
            str(row.get("workload_sequence_diagnostic_signature"))
            for row in input_rows
            if row.get("workload_sequence_diagnostic_signature")
        }
    )
    sequence_diagnostic_match = len(sequence_signatures) == 1
    summary: dict[str, Any] = {
        "sequence_diagnostic_signature_count": len(sequence_signatures),
        "sequence_diagnostic_match": sequence_diagnostic_match,
    }
    if not include_details:
        return summary

    sorted_rows = sorted(input_rows, key=lambda row: str(row.get("config_id") or ""))
    baseline = sorted_rows[0] if sorted_rows else {}
    baseline_events = baseline.get("_workload_sequence_diagnostic_events")
    baseline_sequence = baseline_events if isinstance(baseline_events, list) else []
    mismatches: list[dict[str, Any]] = []
    for row in sorted_rows[1:]:
        current_events = row.get("_workload_sequence_diagnostic_events")
        current_sequence = current_events if isinstance(current_events, list) else []
        mismatch = first_workload_sequence_diagnostic_mismatch(baseline_sequence, current_sequence)
        if mismatch is None:
            continue
        mismatches.append(
            {
                "baseline_config_id": baseline.get("config_id"),
                "config_id": row.get("config_id"),
                **mismatch,
            }
        )

    summary["sequence_diagnostic"] = {
        "enabled": True,
        "match": sequence_diagnostic_match,
        "baseline_config_id": baseline.get("config_id"),
        "mismatch_count": len(mismatches),
        "mismatch_samples": mismatches[:sample_limit],
    }
    return summary


def first_workload_sequence_diagnostic_mismatch(
    source: list[dict[str, Any]],
    target: list[dict[str, Any]],
) -> dict[str, Any] | None:
    """Locate the first ``(role, signature)`` difference between sequences."""

    limit = min(len(source), len(target))
    for index in range(limit):
        if sequence_compare_key(source[index]) != sequence_compare_key(target[index]):
            return {
                "index": index,
                "source": source[index],
                "target": target[index],
            }
    if len(source) != len(target):
        return {
            "index": limit,
            "source": source[limit] if limit < len(source) else None,
            "target": target[limit] if limit < len(target) else None,
        }
    return None


def sequence_compare_key(event: dict[str, Any]) -> tuple[str, str]:
    """Return the strict comparison key for a sequence event."""

    return str(event.get("role") or ""), str(event.get("signature") or "")
