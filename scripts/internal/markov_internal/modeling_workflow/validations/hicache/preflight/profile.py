"""Post-profile HiCache audit used by workflow input preflight.

The profiling frontend only captures artifacts. This module interprets those
artifacts for HiCache consumers and reports state-model readiness, strict
diagnostic coverage, forced-token contracts, and the aggregate workflow gate.
"""

from __future__ import annotations

import json
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from markov_internal.audit.profile_artifacts import (
    audit_profile_artifacts,
    configured_targets,
    discover_workload_report,
    is_python_probe_trace_event,
    load_python_probe_events,
    load_run_config,
)
from markov_internal.common.manifest import existing_manifest_files
from markov_internal.common.io import load_json
from markov_internal.common.paths import map_repo_path
from markov_internal.contracts.forced_token.quality import forced_token_quality_from_workload_report
from ..core.facts import HICACHE_CONSUMER_STATE_MODEL, parse_fact_or_none
from ..oracle.evidence.capacity_values import HiCacheCapacityEvidence
from .mechanisms import (
    configured_mechanisms,
    observe_mechanism,
)
from .state_fact_accumulator import HiCacheStateFactAccumulator


@dataclass(frozen=True)
class HiCacheProfileAuditOptions:
    """Select optional evidence families evaluated by a profile audit."""

    validate_forced_token: bool = False
    validate_oracle_evidence: bool = False
    validate_diagnostic_coverage: bool = False


@dataclass(frozen=True)
class _ObservedProfileEvidence:
    """Evidence accumulated during the single pass over Python probe events."""

    mechanism_counts: Counter[str]
    state_fact_coverage: dict[str, Any]
    capacity_evidence: HiCacheCapacityEvidence


@dataclass(frozen=True)
class _ForcedTokenAudit:
    """Optional forced-token contract result with stable disabled values."""

    expected_mode: str | None
    quality: dict[str, Any] | None
    errors: list[str]
    ready: bool | None


@dataclass(frozen=True)
class _DiagnosticCoverageAudit:
    """Optional strict mechanism-coverage result for diagnostic workflows."""

    configured_mechanisms: list[str]
    expected_mechanisms: list[str]
    expected_configured_mechanisms: list[str]
    missing_mechanisms: list[str]
    errors: list[str]


@dataclass(frozen=True)
class _StateModelInputAudit:
    """State-model readiness derived from generic artifacts and routed facts."""

    errors: list[str]
    blocking_artifact_errors: list[str]
    ready: bool


def audit_hicache_profile(
    manifest_path: Path,
    *,
    options: HiCacheProfileAuditOptions | None = None,
    event_rows: list[tuple[Path, dict[str, Any]]] | None = None,
) -> dict[str, Any]:
    """Audit one profile run for the selected HiCache workflow consumers."""

    options = options or HiCacheProfileAuditOptions()
    manifest = load_json(manifest_path)
    if not isinstance(manifest, dict):
        raise ValueError(f"profile manifest must be a JSON object: {manifest_path}")
    run_dir = map_repo_path(Path(str(manifest.get("run_dir") or manifest_path.parent)))
    profiling = manifest.get("profiling") if isinstance(manifest.get("profiling"), dict) else {}
    sidecar = manifest.get("sidecar") if isinstance(manifest.get("sidecar"), dict) else {}
    preloaded_events = (
        [event for _path, event in event_rows if is_python_probe_trace_event(event)] if event_rows is not None else None
    )
    artifact_audit = audit_profile_artifacts(manifest_path, python_probe_events=preloaded_events)
    artifact_errors = [str(error) for error in artifact_audit.get("artifact_errors", [])]
    artifact_ready = bool(artifact_audit.get("artifact_ready"))

    requested_consumers = {
        str(consumer) for consumer in profiling.get("python_consumers") or [] if isinstance(consumer, str)
    }
    hicache_state_model_enabled = HICACHE_CONSUMER_STATE_MODEL in requested_consumers

    python_probe_files = existing_manifest_files(sidecar.get("python_probe_files", []))
    events = preloaded_events if preloaded_events is not None else load_python_probe_events(python_probe_files)
    observed = _observe_profile_events(events, options)

    workload_report = (
        discover_workload_report(run_dir)
        if options.validate_forced_token or options.validate_diagnostic_coverage
        else None
    )
    forced_token = _audit_forced_token(
        load_run_config(manifest, run_dir) if options.validate_forced_token else {},
        workload_report,
        enabled=options.validate_forced_token,
    )
    diagnostic = _audit_diagnostic_coverage(
        profiling,
        workload_report,
        observed.mechanism_counts,
        enabled=options.validate_diagnostic_coverage,
    )
    state_model = _audit_state_model_input(
        observed.state_fact_coverage,
        artifact_errors,
        enabled=hicache_state_model_enabled,
    )
    validator_evidence_errors = _validator_evidence_errors(
        observed.capacity_evidence,
        enabled=options.validate_oracle_evidence,
    )
    validator_evidence_ready = (
        not state_model.blocking_artifact_errors and not validator_evidence_errors
        if options.validate_oracle_evidence
        else None
    )
    strict_diagnostic_coverage_ready = (
        artifact_ready and not diagnostic.errors if options.validate_diagnostic_coverage else None
    )
    workflow_input_ready = state_model.ready
    if options.validate_forced_token:
        workflow_input_ready = workflow_input_ready and bool(forced_token.ready)
    if options.validate_oracle_evidence:
        workflow_input_ready = workflow_input_ready and bool(validator_evidence_ready)

    result = {
        **artifact_audit,
        "schema": "trace_sim.hicache_profile_audit.v1",
        "audit_options": {
            "validate_forced_token": options.validate_forced_token,
            "validate_oracle_evidence": options.validate_oracle_evidence,
            "validate_diagnostic_coverage": options.validate_diagnostic_coverage,
        },
        "requested_consumers": sorted(requested_consumers),
        "hicache_state_model_enabled": hicache_state_model_enabled,
        "hicache_state_model_fact_coverage": observed.state_fact_coverage,
        "state_model_input_errors": state_model.errors,
        "state_model_input_artifact_errors": state_model.blocking_artifact_errors,
        "state_model_input_ready": state_model.ready,
        "artifact_errors": artifact_errors,
        "artifact_ready": artifact_ready,
        "workflow_input_errors": sorted(
            set(
                state_model.blocking_artifact_errors
                + state_model.errors
                + validator_evidence_errors
                + (forced_token.errors if options.validate_forced_token else [])
            )
        ),
        "workflow_input_ready": workflow_input_ready,
    }
    if options.validate_forced_token:
        result.update(
            {
                "workload_report": str(workload_report) if workload_report else None,
                "expected_forced_token_mode": forced_token.expected_mode,
                "forced_token_quality": forced_token.quality,
                "forced_token_errors": forced_token.errors,
                "forced_token_ready": forced_token.ready,
            }
        )
    if options.validate_diagnostic_coverage:
        result.update(
            {
                "workload_report": str(workload_report) if workload_report else None,
                "expected_cache_mechanisms": diagnostic.expected_mechanisms,
                "configured_cache_mechanisms": diagnostic.configured_mechanisms,
                "expected_configured_cache_mechanisms": diagnostic.expected_configured_mechanisms,
                "observed_cache_mechanisms": dict(sorted(observed.mechanism_counts.items())),
                "missing_cache_mechanisms": diagnostic.missing_mechanisms,
                "diagnostic_coverage_errors": diagnostic.errors,
                "strict_diagnostic_coverage_ready": strict_diagnostic_coverage_ready,
            }
        )
    if options.validate_oracle_evidence:
        result.update(
            {
                "validator_evidence_ready": validator_evidence_ready,
                "validator_evidence_errors": validator_evidence_errors,
                "hicache_capacity_observed": observed.capacity_evidence.snapshot_count > 0,
                "hicache_capacity": observed.capacity_evidence.as_payload(),
            }
        )
    return result


def _observe_profile_events(
    events: list[dict[str, Any]], options: HiCacheProfileAuditOptions
) -> _ObservedProfileEvidence:
    """Scan Python probe events once for the selected evidence families."""

    mechanism_counts: Counter[str] = Counter()
    state_fact_accumulator = HiCacheStateFactAccumulator()
    capacity_evidence = HiCacheCapacityEvidence()
    for event in events:
        raw_args = event.get("args")
        if not isinstance(raw_args, dict):
            continue
        if options.validate_oracle_evidence:
            fact = parse_fact_or_none(raw_args)
            snapshot = raw_args.get("state_snapshot")
            if (
                fact is not None
                and fact.fact_class == "oracle_state"
                and fact.role == "state_snapshot"
                and isinstance(snapshot, dict)
            ):
                capacity_evidence.observe_snapshot(snapshot)
        if options.validate_diagnostic_coverage:
            observe_mechanism(mechanism_counts, raw_args)
        state_fact_accumulator.observe(raw_args)
    return _ObservedProfileEvidence(
        mechanism_counts=mechanism_counts,
        state_fact_coverage=state_fact_accumulator.finalize(),
        capacity_evidence=capacity_evidence,
    )


def _audit_forced_token(
    run_config: dict[str, Any],
    workload_report: Path | None,
    *,
    enabled: bool,
) -> _ForcedTokenAudit:
    """Evaluate the optional forced-token mode and workload-report contract."""

    if not enabled:
        return _ForcedTokenAudit(expected_mode=None, quality=None, errors=[], ready=None)

    expected_mode = _expected_forced_token_mode(run_config)
    quality = forced_token_quality_from_workload_report(workload_report)
    errors: list[str] = []
    if expected_mode:
        if workload_report is None:
            errors.append("forced_token_workload_report_missing")
        if not quality.get("enabled") or quality.get("mode") != expected_mode:
            errors.append("forced_token_mode_mismatch")
    errors.extend(str(error) for error in quality.get("errors", []))
    errors = sorted(set(errors))
    return _ForcedTokenAudit(
        expected_mode=expected_mode,
        quality=quality,
        errors=errors,
        ready=not errors and bool(quality.get("ready", True)),
    )


def _audit_diagnostic_coverage(
    profiling: dict[str, Any],
    workload_report: Path | None,
    mechanism_counts: Counter[str],
    *,
    enabled: bool,
) -> _DiagnosticCoverageAudit:
    """Compare expected and observed HiCache diagnostic mechanisms."""

    if not enabled:
        return _DiagnosticCoverageAudit([], [], [], [], [])

    configured = configured_mechanisms(configured_targets(profiling))
    expected = _expected_mechanisms_from_workload(workload_report)
    expected_configured = sorted(set(expected) & set(configured))
    missing = sorted(mechanism for mechanism in expected_configured if mechanism_counts.get(mechanism, 0) <= 0)
    errors = ["expected_hicache_mechanisms_missing"] if missing else []
    return _DiagnosticCoverageAudit(configured, expected, expected_configured, missing, errors)


def _audit_state_model_input(
    coverage: dict[str, Any],
    artifact_errors: list[str],
    *,
    enabled: bool,
) -> _StateModelInputAudit:
    """Apply state-model fact invariants without inspecting unrelated events."""

    errors: list[str] = []
    if enabled:
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
    errors = sorted(set(errors))
    blocking_artifact_errors = _state_blocking_artifact_errors(artifact_errors)
    return _StateModelInputAudit(
        errors=errors,
        blocking_artifact_errors=blocking_artifact_errors,
        ready=not blocking_artifact_errors and not errors,
    )


def _validator_evidence_errors(capacity_evidence: HiCacheCapacityEvidence, *, enabled: bool) -> list[str]:
    """Return the optional oracle-capacity evidence blocker list."""

    if enabled and capacity_evidence.snapshot_count <= 0:
        return ["hicache_capacity_snapshot_missing"]
    return []


def _expected_forced_token_mode(config: dict[str, Any]) -> str | None:
    """Read the non-degradable forced-token mode from an expanded run config."""

    metadata = config.get("metadata") if isinstance(config.get("metadata"), dict) else {}
    profile_mode = metadata.get("profile_mode")
    if profile_mode == "forced_token_capture":
        return "capture"
    if profile_mode == "forced_token_replay":
        return "replay"
    return None


def _state_blocking_artifact_errors(artifact_errors: list[str]) -> list[str]:
    """Select generic artifact errors that invalidate state-model input."""

    blocking = {
        "missing_python_probe_files",
        "all_python_probe_targets_missing",
        "python_probe_exception_events",
    }
    return sorted(error for error in artifact_errors if error in blocking)


def _expected_mechanisms_from_workload(path: Path | None) -> list[str]:
    """Read expected HiCache mechanism coverage from a workload report."""

    if path is None or not path.is_file():
        return []
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return []
    raw = report.get("expected_cache_mechanisms")
    mechanisms: set[str] = set()
    if isinstance(raw, dict):
        for value in raw.values():
            if isinstance(value, list):
                mechanisms.update(str(item) for item in value)
    elif isinstance(raw, list):
        mechanisms.update(str(item) for item in raw)
    return sorted(mechanisms)
