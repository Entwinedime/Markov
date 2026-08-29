"""Readiness of source HiCache facts consumed by the C++ model."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from markov_internal.audit.profile_artifacts import (
    audit_profile_artifacts,
    is_python_probe_trace_event,
    load_python_probe_events,
)
from markov_internal.common.io import load_json
from markov_internal.common.manifest import existing_manifest_files

from ..core.facts import HICACHE_CONSUMER_STATE_MODEL
from .state_fact_accumulator import HiCacheStateFactAccumulator


def audit_hicache_profile(manifest_path: Path) -> dict[str, Any]:
    """Check only artifacts and state facts required by source replay."""

    manifest = load_json(manifest_path)
    if not isinstance(manifest, dict):
        raise ValueError(f"profile manifest must be a JSON object: {manifest_path}")
    profiling = manifest.get("profiling") if isinstance(manifest.get("profiling"), dict) else {}
    sidecar = manifest.get("sidecar") if isinstance(manifest.get("sidecar"), dict) else {}
    requested_consumers = {
        str(consumer) for consumer in profiling.get("python_consumers") or [] if isinstance(consumer, str)
    }
    state_model_enabled = HICACHE_CONSUMER_STATE_MODEL in requested_consumers
    probe_files = existing_manifest_files(sidecar.get("python_probe_files", []))
    events = [event for event in load_python_probe_events(probe_files) if is_python_probe_trace_event(event)]
    artifact_audit = audit_profile_artifacts(manifest_path, python_probe_events=events)
    artifact_errors = [str(error) for error in artifact_audit.get("artifact_errors", [])]
    coverage = state_fact_coverage(events)
    state_errors = state_fact_errors(coverage, enabled=state_model_enabled)
    blocking_artifact_errors = sorted(
        error
        for error in artifact_errors
        if error in {"missing_python_probe_files", "all_python_probe_targets_missing", "python_probe_exception_events"}
    )
    workflow_errors = sorted(set(blocking_artifact_errors + state_errors))
    return {
        **artifact_audit,
        "requested_consumers": sorted(requested_consumers),
        "hicache_state_model_enabled": state_model_enabled,
        "hicache_state_model_fact_coverage": coverage,
        "state_model_input_errors": state_errors,
        "state_model_input_artifact_errors": blocking_artifact_errors,
        "state_model_input_ready": not workflow_errors,
        "artifact_errors": artifact_errors,
        "artifact_ready": bool(artifact_audit.get("artifact_ready")),
        "workflow_input_errors": workflow_errors,
        "workflow_input_ready": not workflow_errors,
    }


def state_fact_coverage(events: list[dict[str, Any]]) -> dict[str, Any]:
    accumulator = HiCacheStateFactAccumulator()
    for event in events:
        args = event.get("args")
        if isinstance(args, dict):
            accumulator.observe(args)
    return accumulator.finalize()


def state_fact_errors(coverage: dict[str, Any], *, enabled: bool) -> list[str]:
    if not enabled:
        return ["hicache_state_model_consumer_not_enabled"]
    errors: list[str] = []
    if coverage["missing_required_fact_events"] > 0:
        errors.append("hicache_state_model_facts_missing")
    if coverage["route_error_events"] > 0:
        errors.append("hicache_state_fact_route_invalid")
    if coverage["missing_token_dictionary_refs"] or coverage["dictionary_ids_without_tokens"]:
        errors.append("hicache_token_dictionary_missing")
    if coverage["invalid_token_dictionary_issue_count"] > 0:
        errors.append("hicache_token_dictionary_invalid")
    if coverage["seq_order_error_count"] > 0:
        errors.append("hicache_state_fact_seq_invalid")
    return sorted(set(errors))
