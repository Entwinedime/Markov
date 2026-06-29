#!/usr/bin/env python3
"""HiCache profiling 后审计与 workflow input readiness。

profiling 前端只负责采集 artifact。本模块面向 HiCache consumer 解释这些
artifact，输出 state-model input readiness、strict diagnostic coverage、
forced-token replay/capture contract 和 workflow gate。
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path
from typing import Any

from ...audit.profile_artifacts import (
    audit_profile_artifacts,
    configured_targets,
    discover_workload_report,
    existing_paths,
    load_python_probe_events,
    load_run_config,
    resolve_path,
)
from ...common.paths import map_repo_path
from ...contracts.forced_token import forced_token_quality_from_workload_report
from ..facts import (
    HICACHE_CONSUMER_FINAL_STATE_VALIDATOR,
    HICACHE_CONSUMER_STATE_MODEL,
    HICACHE_CONSUMER_TRANSITION_VALIDATOR,
)
from .state_facts import (
    configured_fact_role,
    configured_mechanisms,
    finalize_hicache_capacity,
    finalize_hicache_state_facts,
    new_hicache_capacity_accumulator,
    new_hicache_state_fact_accumulator,
    observe_hicache_capacity,
    observe_hicache_state_fact,
    observe_mechanism,
)


def main(argv: list[str] | None = None) -> int:
    """HiCache profile audit 的 CLI 入口。"""

    args = parse_args(argv)
    manifest_path = resolve_path(args.manifest)
    result = audit_hicache_profile(manifest_path)
    output_path = resolve_output_path(args.output, manifest_path, result)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    sys.stdout.write(str(output_path) + "\n")
    return 0 if result.get("workflow_input_ready") else 1


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """解析 HiCache profile audit 参数。"""

    parser = argparse.ArgumentParser(description="Audit HiCache post-profile readiness.")
    parser.add_argument("--manifest", required=True, help="profile_manifest.json path")
    parser.add_argument(
        "--output",
        help="output JSON path; defaults to run_dir/hicache_profile_audit.json",
    )
    return parser.parse_args(argv)


def audit_hicache_profile(manifest_path: Path) -> dict[str, Any]:
    """面向 HiCache workflow consumer 审计单个 profile run。"""

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    run_dir = map_repo_path(Path(str(manifest.get("run_dir") or manifest_path.parent)))
    run_config = load_run_config(manifest, run_dir)
    profiling = manifest.get("profiling") if isinstance(manifest.get("profiling"), dict) else {}
    sidecar = manifest.get("sidecar") if isinstance(manifest.get("sidecar"), dict) else {}
    artifact_audit = audit_profile_artifacts(manifest_path)
    artifact_errors = [str(error) for error in artifact_audit.get("artifact_errors", [])]
    artifact_ready = bool(artifact_audit.get("artifact_ready"))

    configured = configured_targets(profiling)
    requested_consumers = {
        str(consumer)
        for consumer in profiling.get("python_consumers") or []
        if isinstance(consumer, str)
    }
    hicache_state_model_enabled = HICACHE_CONSUMER_STATE_MODEL in requested_consumers
    hicache_state_trace_enabled = bool(
        requested_consumers
        & {HICACHE_CONSUMER_FINAL_STATE_VALIDATOR, HICACHE_CONSUMER_TRANSITION_VALIDATOR}
    )
    lifecycle_observed_configured = any(
        configured_fact_role(target) == "request_lifecycle_path_observed"
        for target in configured.values()
    )

    python_probe_files = existing_paths(sidecar.get("python_probe_files", []))
    events = load_python_probe_events(python_probe_files)
    mechanism_counts: Counter[str] = Counter()
    state_fact_accumulator = new_hicache_state_fact_accumulator()
    capacity_accumulator = new_hicache_capacity_accumulator()
    for event in events:
        raw_args = event.get("args")
        if not isinstance(raw_args, dict):
            continue
        observe_hicache_capacity(capacity_accumulator, raw_args)
        observe_mechanism(mechanism_counts, raw_args)
        observe_hicache_state_fact(state_fact_accumulator, raw_args)

    workload_report = discover_workload_report(run_dir)
    expected_forced_token_mode = _expected_forced_token_mode(run_config)
    forced_token_quality = forced_token_quality_from_workload_report(workload_report)
    forced_token_errors: list[str] = []
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
    forced_token_ready = not forced_token_errors and bool(forced_token_quality.get("ready", True))

    configured_mechanism_names = configured_mechanisms(configured)
    expected_mechanisms = _expected_mechanisms_from_workload(workload_report)
    expected_configured_mechanisms = sorted(set(expected_mechanisms) & set(configured_mechanism_names))
    missing_mechanisms = sorted(
        mechanism
        for mechanism in expected_configured_mechanisms
        if mechanism_counts.get(mechanism, 0) <= 0
    )
    diagnostic_coverage_errors: list[str] = []
    if missing_mechanisms:
        diagnostic_coverage_errors.append("expected_hicache_mechanisms_missing")

    state_model_input_errors: list[str] = []
    state_fact_coverage = finalize_hicache_state_facts(state_fact_accumulator)
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
        if state_fact_coverage.get("prefetch_path_contract_error_count", 0) > 0:
            state_model_input_errors.append("hicache_prefetch_decision_path_contract_invalid")
        if lifecycle_observed_configured and state_fact_coverage["lifecycle_path_contract_error_count"] > 0:
            state_model_input_errors.append("hicache_lifecycle_path_contract_invalid")

    hicache_capacity = finalize_hicache_capacity(capacity_accumulator)
    if hicache_state_trace_enabled and capacity_accumulator["snapshot_count"] <= 0:
        state_model_input_errors.append("hicache_capacity_snapshot_missing")

    state_model_input_errors = sorted(set(state_model_input_errors))
    state_blocking_artifact_errors = _state_blocking_artifact_errors(artifact_errors)
    state_model_input_ready = not state_blocking_artifact_errors and not state_model_input_errors and forced_token_ready
    strict_diagnostic_coverage_ready = artifact_ready and not diagnostic_coverage_errors
    workflow_input_ready = state_model_input_ready and forced_token_ready

    return {
        **artifact_audit,
        "schema": "trace_sim.hicache_profile_audit.v1",
        "workload_report": str(workload_report) if workload_report else None,
        "expected_forced_token_mode": expected_forced_token_mode,
        "forced_token_quality": forced_token_quality,
        "forced_token_errors": forced_token_errors,
        "forced_token_ready": forced_token_ready,
        "expected_cache_mechanisms": expected_mechanisms,
        "configured_cache_mechanisms": configured_mechanism_names,
        "expected_configured_cache_mechanisms": expected_configured_mechanisms,
        "observed_cache_mechanisms": dict(sorted(mechanism_counts.items())),
        "missing_cache_mechanisms": missing_mechanisms,
        "diagnostic_coverage_errors": diagnostic_coverage_errors,
        "strict_diagnostic_coverage_ready": strict_diagnostic_coverage_ready,
        "hicache_state_model_enabled": hicache_state_model_enabled,
        "lifecycle_observed_configured": lifecycle_observed_configured,
        "hicache_state_trace_enabled": hicache_state_trace_enabled,
        "hicache_capacity_observed": capacity_accumulator["snapshot_count"] > 0,
        "hicache_capacity": hicache_capacity,
        "hicache_state_model_fact_coverage": state_fact_coverage,
        "state_model_input_errors": state_model_input_errors,
        "state_model_input_artifact_errors": state_blocking_artifact_errors,
        "state_model_input_ready": state_model_input_ready,
        "artifact_errors": artifact_errors,
        "artifact_ready": artifact_ready,
        "workflow_input_errors": sorted(
            set(state_blocking_artifact_errors + state_model_input_errors + forced_token_errors)
        ),
        "workflow_input_ready": workflow_input_ready,
    }


def resolve_output_path(value: str | None, manifest_path: Path, result: dict[str, Any]) -> Path:
    """解析 HiCache profile audit 输出路径。"""

    if value:
        return resolve_path(value)
    run_dir = Path(str(result.get("run_dir") or manifest_path.parent))
    return run_dir / "hicache_profile_audit.json"


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


if __name__ == "__main__":
    raise SystemExit(main())
