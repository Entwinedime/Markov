"""HiCache profiling 后审计与 workflow input preflight。

profiling 前端只负责采集 artifact。本模块面向 HiCache consumer 解释这些
artifact，输出 state-model input ready 状态、strict diagnostic coverage、
forced-token replay/capture contract 和 workflow gate。
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
    existing_paths,
    load_python_probe_events,
    load_run_config,
)
from markov_internal.common.paths import map_repo_path
from markov_internal.contracts.forced_token import forced_token_quality_from_workload_report
from ..core.facts import HICACHE_CONSUMER_STATE_MODEL
from .capacity_accumulator import HiCacheCapacityAccumulator
from .mechanisms import (
    configured_mechanisms,
    observe_mechanism,
)
from .state_fact_accumulator import HiCacheStateFactAccumulator


@dataclass(frozen=True)
class HiCacheProfileAuditOptions:
    """HiCache profile audit 的执行路径开关。"""

    validate_forced_token: bool = False
    validate_oracle_evidence: bool = False
    validate_diagnostic_coverage: bool = False


def audit_hicache_profile(
    manifest_path: Path,
    *,
    options: HiCacheProfileAuditOptions | None = None,
) -> dict[str, Any]:
    """面向 HiCache workflow consumer 审计单个 profile run。"""

    options = options or HiCacheProfileAuditOptions()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    run_dir = map_repo_path(Path(str(manifest.get("run_dir") or manifest_path.parent)))
    run_config = load_run_config(manifest, run_dir) if options.validate_forced_token else {}
    profiling = manifest.get("profiling") if isinstance(manifest.get("profiling"), dict) else {}
    sidecar = manifest.get("sidecar") if isinstance(manifest.get("sidecar"), dict) else {}
    artifact_audit = audit_profile_artifacts(manifest_path)
    artifact_errors = [str(error) for error in artifact_audit.get("artifact_errors", [])]
    artifact_ready = bool(artifact_audit.get("artifact_ready"))

    configured = configured_targets(profiling) if options.validate_diagnostic_coverage else {}
    requested_consumers = {
        str(consumer) for consumer in profiling.get("python_consumers") or [] if isinstance(consumer, str)
    }
    hicache_state_model_enabled = HICACHE_CONSUMER_STATE_MODEL in requested_consumers
    validator_evidence_requested = options.validate_oracle_evidence

    python_probe_files = existing_paths(sidecar.get("python_probe_files", []))
    events = load_python_probe_events(python_probe_files)
    mechanism_counts: Counter[str] = Counter()
    state_fact_accumulator = HiCacheStateFactAccumulator()
    capacity_accumulator = HiCacheCapacityAccumulator()
    for event in events:
        raw_args = event.get("args")
        if not isinstance(raw_args, dict):
            continue
        if options.validate_oracle_evidence:
            capacity_accumulator.observe(raw_args)
        if options.validate_diagnostic_coverage:
            observe_mechanism(mechanism_counts, raw_args)
        state_fact_accumulator.observe(raw_args)

    workload_report = (
        discover_workload_report(run_dir)
        if options.validate_forced_token or options.validate_diagnostic_coverage
        else None
    )
    expected_forced_token_mode = _expected_forced_token_mode(run_config) if options.validate_forced_token else None
    forced_token_errors: list[str] = []
    forced_token_quality: dict[str, Any] | None = None
    if options.validate_forced_token:
        forced_token_quality = forced_token_quality_from_workload_report(workload_report)
        if expected_forced_token_mode:
            if workload_report is None:
                forced_token_errors.append("forced_token_workload_report_missing")
            if (
                not forced_token_quality.get("enabled")
                or forced_token_quality.get("mode") != expected_forced_token_mode
            ):
                forced_token_errors.append("forced_token_mode_mismatch")
        forced_token_errors.extend(str(error) for error in forced_token_quality.get("errors", []))
    forced_token_errors = sorted(set(forced_token_errors))
    forced_token_ready = (
        not forced_token_errors and bool(forced_token_quality.get("ready", True))
        if options.validate_forced_token and forced_token_quality is not None
        else None
    )

    diagnostic_coverage_errors: list[str] = []
    if options.validate_diagnostic_coverage:
        configured_mechanism_names = configured_mechanisms(configured)
        expected_mechanisms = _expected_mechanisms_from_workload(workload_report)
        expected_configured_mechanisms = sorted(set(expected_mechanisms) & set(configured_mechanism_names))
        missing_mechanisms = sorted(
            mechanism for mechanism in expected_configured_mechanisms if mechanism_counts.get(mechanism, 0) <= 0
        )
        if missing_mechanisms:
            diagnostic_coverage_errors.append("expected_hicache_mechanisms_missing")
    else:
        configured_mechanism_names = []
        expected_mechanisms = []
        expected_configured_mechanisms = []
        missing_mechanisms = []

    state_model_input_errors: list[str] = []
    state_fact_coverage = state_fact_accumulator.finalize()
    if hicache_state_model_enabled:
        if (
            state_fact_coverage["missing_required_fact_events"] > 0
            or state_fact_coverage.get("missing_required_role_count", 0) > 0
        ):
            state_model_input_errors.append("hicache_state_model_facts_missing")
        if state_fact_coverage["route_error_events"] > 0:
            state_model_input_errors.append("hicache_state_fact_route_invalid")
        if state_fact_coverage["missing_token_dictionary_refs"] or state_fact_coverage["dictionary_ids_without_tokens"]:
            state_model_input_errors.append("hicache_token_dictionary_missing")
        if state_fact_coverage["invalid_token_dictionary_issue_count"] > 0:
            state_model_input_errors.append("hicache_token_dictionary_invalid")
        if state_fact_coverage["seq_order_error_count"] > 0:
            state_model_input_errors.append("hicache_state_fact_seq_invalid")
    hicache_capacity = capacity_accumulator.finalize() if options.validate_oracle_evidence else None
    validator_evidence_errors: list[str] = []
    if validator_evidence_requested and capacity_accumulator.snapshot_count <= 0:
        validator_evidence_errors.append("hicache_capacity_snapshot_missing")

    state_model_input_errors = sorted(set(state_model_input_errors))
    validator_evidence_errors = sorted(set(validator_evidence_errors))
    state_blocking_artifact_errors = _state_blocking_artifact_errors(artifact_errors)
    state_model_input_ready = not state_blocking_artifact_errors and not state_model_input_errors
    validator_evidence_ready = (
        not state_blocking_artifact_errors and not validator_evidence_errors if validator_evidence_requested else None
    )
    strict_diagnostic_coverage_ready = (
        artifact_ready and not diagnostic_coverage_errors if options.validate_diagnostic_coverage else None
    )
    workflow_input_ready = state_model_input_ready
    if options.validate_forced_token:
        workflow_input_ready = workflow_input_ready and bool(forced_token_ready)
    if validator_evidence_requested:
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
        "hicache_state_model_fact_coverage": state_fact_coverage,
        "state_model_input_errors": state_model_input_errors,
        "state_model_input_artifact_errors": state_blocking_artifact_errors,
        "state_model_input_ready": state_model_input_ready,
        "artifact_errors": artifact_errors,
        "artifact_ready": artifact_ready,
        "workflow_input_errors": sorted(
            set(
                state_blocking_artifact_errors
                + state_model_input_errors
                + validator_evidence_errors
                + (forced_token_errors if options.validate_forced_token else [])
            )
        ),
        "workflow_input_ready": workflow_input_ready,
    }
    if options.validate_forced_token:
        result.update(
            {
                "workload_report": str(workload_report) if workload_report else None,
                "expected_forced_token_mode": expected_forced_token_mode,
                "forced_token_quality": forced_token_quality,
                "forced_token_errors": forced_token_errors,
                "forced_token_ready": forced_token_ready,
            }
        )
    if options.validate_diagnostic_coverage:
        result.update(
            {
                "workload_report": str(workload_report) if workload_report else None,
                "expected_cache_mechanisms": expected_mechanisms,
                "configured_cache_mechanisms": configured_mechanism_names,
                "expected_configured_cache_mechanisms": expected_configured_mechanisms,
                "observed_cache_mechanisms": dict(sorted(mechanism_counts.items())),
                "missing_cache_mechanisms": missing_mechanisms,
                "diagnostic_coverage_errors": diagnostic_coverage_errors,
                "strict_diagnostic_coverage_ready": strict_diagnostic_coverage_ready,
            }
        )
    if validator_evidence_requested:
        result.update(
            {
                "validator_evidence_ready": validator_evidence_ready,
                "validator_evidence_errors": validator_evidence_errors,
                "hicache_capacity_observed": capacity_accumulator.snapshot_count > 0,
                "hicache_capacity": hicache_capacity,
            }
        )
    return result


def _expected_forced_token_mode(config: dict[str, Any]) -> str | None:
    """从 expanded run config 读取不可降级的 forced-token 模式。"""

    metadata = config.get("metadata") if isinstance(config.get("metadata"), dict) else {}
    profile_mode = metadata.get("profile_mode")
    if profile_mode == "forced_token_capture":
        return "capture"
    if profile_mode == "forced_token_replay":
        return "replay"
    return None


def _state_blocking_artifact_errors(artifact_errors: list[str]) -> list[str]:
    """筛出会让 state-model input 不可用的通用 artifact 错误。"""

    blocking = {
        "missing_python_probe_files",
        "all_python_probe_targets_missing",
        "python_probe_exception_events",
    }
    return sorted(error for error in artifact_errors if error in blocking)


def _expected_mechanisms_from_workload(path: Path | None) -> list[str]:
    """从 workload report 读取期望覆盖的 HiCache 机制。"""

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
