"""Build backend validation artifacts for one modeling run."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from markov_internal.common.io import load_json, write_json
from markov_internal.common.paths import require_repo_path
from markov_internal.modeling.workload import WorkloadWindow, optional_float

from ..oracle.diff.event_delta import build_event_delta_validation
from ..oracle.diff.mismatch import first_hicache_mismatch
from ..oracle.diff.timeline_delta import build_timeline_delta_validation
from ..oracle.evidence.capacity import (
    build_hicache_capacity_config_audit,
    extract_hicache_capacity_oracle_state,
    observed_max_derived_state_counts,
)
from ..oracle.evidence.coverage import (
    build_request_transition_coverage,
    build_transition_coverage,
)
from ..oracle.snapshot.records import load_predicted_state_records
from ..oracle.snapshot.state import (
    diff_hicache_sets,
    extract_hicache_state_snapshots,
    final_state_counts,
    latest_derived_state,
    normalize_hicache_state_for_oracle_compare,
    unchecked_model_state_keys,
)
from .prediction import extract_hicache_summary


def build_validation(
    mode: str,
    prediction: dict[str, Any],
    run_summary: dict[str, Any],
    workload_window: WorkloadWindow | None,
    trace_paths: list[Path],
    config: dict[str, Any],
    module_summary_path: Path,
    predicted_state_trace_path: Path | None,
    hicache_oracle_trace_paths: list[Path] | None = None,
) -> dict[str, Any]:
    """Build the complete validation document for one modeling run."""

    validation_config = config.get("validation") if isinstance(config.get("validation"), dict) else {}
    threshold = float(validation_config.get("faithful_replay_full_e2e_rel_error_max", 0.05))
    summary_real = optional_float(run_summary.get("real_e2e_us"))
    actual = int(summary_real) if summary_real and summary_real > 0 else None
    predicted = int(prediction["predicted_e2e_us"])
    relative_error = abs(predicted - actual) / actual if actual else None
    errors = faithful_replay_errors(mode, actual, relative_error, threshold)

    result = {
        "mode": mode,
        "engine": "cpp_trace_graph",
        "validation_ready": not errors,
        "validation_errors": errors,
        "thresholds": {"faithful_replay_full_e2e_rel_error_max": threshold},
        "trace_files": [str(path) for path in trace_paths],
        "dag": {
            "node_count": run_summary.get("node_count"),
            "edge_count": run_summary.get("edge_count"),
            "parsed_record_count": run_summary.get("parsed_record_count"),
            "edge_counts_by_kind": run_summary.get("edge_counts_by_kind"),
            "dag_mutation_count": hicache_dag_mutation_count(run_summary),
        },
        "workload_window": workload_window_payload(workload_window),
        "e2e": {
            "predicted_e2e_us": predicted,
            "actual_e2e_us": actual,
            "actual_source": "trace_real_e2e_us" if actual is not None else None,
            "absolute_error_us": predicted - actual if actual else None,
            "relative_error": relative_error,
        },
    }

    hicache = build_hicache_state_validation(
        validation_config,
        trace_paths,
        module_summary_path,
        predicted_state_trace_path,
        hicache_oracle_trace_paths or [],
    )
    if hicache is not None:
        result["hicache_state"] = hicache
        errors.extend(hicache_validation_errors(hicache))
        result["validation_errors"] = errors
        result["validation_ready"] = not errors
    return result


def hicache_dag_mutation_count(run_summary: dict[str, Any]) -> int:
    """Read the applied mutation count from the C++ patch-module result."""

    module_results = run_summary.get("module_results")
    patch = module_results.get("hicache_dag_patch") if isinstance(module_results, dict) else None
    if not isinstance(patch, dict):
        return 0
    try:
        return int(patch.get("mutation_count") or 0)
    except (TypeError, ValueError):
        return 0


def faithful_replay_errors(
    mode: str,
    actual: int | None,
    relative_error: float | None,
    threshold: float,
) -> list[str]:
    """Evaluate faithful-replay prerequisites and the configured E2E gate."""

    errors: list[str] = []
    if mode == "faithful_replay" and actual is None:
        errors.append("missing_trace_real_e2e")
    if mode == "faithful_replay" and relative_error is not None and relative_error > threshold:
        errors.append("faithful_replay_full_e2e_error_too_high")
    return errors


def workload_window_payload(window: WorkloadWindow | None) -> dict[str, Any]:
    """Serialize optional workload-window provenance for validation output."""

    return {
        "used": window is not None,
        "report_path": str(window.report_path) if window else None,
        "source": window.source if window else None,
        "actual_e2e_ns": window.actual_e2e_ns if window else None,
    }


def hicache_validation_errors(validation: dict[str, Any]) -> list[str]:
    """Translate HiCache readiness and exactness failures to stable errors."""

    errors: list[str] = []
    if validation.get("oracle_state_validation_required", False):
        if not validation.get("state_trace_ready", False):
            errors.append("hicache_state_trace_not_ready")
        elif validation.get("final_state_match") is False:
            errors.append("hicache_final_state_mismatch")
    if not validation.get("state_model_fact_ready", False):
        errors.append("hicache_state_model_fact_not_ready")
    return errors


def build_hicache_state_validation(
    validation_config: dict[str, Any],
    trace_paths: list[Path],
    module_summary_path: Path,
    predicted_state_trace_path: Path | None,
    oracle_trace_paths_override: list[Path],
) -> dict[str, Any] | None:
    """Build oracle comparisons when HiCache state validation is enabled."""

    hicache_config = (
        validation_config.get("hicache_state") if isinstance(validation_config.get("hicache_state"), dict) else {}
    )
    if not bool(hicache_config.get("enabled", False)):
        return None

    oracle_paths = oracle_trace_paths_override or configured_oracle_paths(hicache_config) or trace_paths
    model_summary = load_json(module_summary_path) if module_summary_path.is_file() else {}
    hicache_summary = extract_hicache_summary(model_summary)
    snapshots = extract_hicache_state_snapshots(oracle_paths)
    oracle_final = latest_derived_state(snapshots)
    model_final = hicache_summary.get("final_state") if isinstance(hicache_summary.get("final_state"), dict) else {}
    page_key_mode = str(hicache_config.get("oracle_page_key_mode") or "strip_scope")
    ignored_state_keys = configured_ignore_state_keys(hicache_config)

    raw_diff = diff_hicache_sets(model_final, oracle_final)
    raw_active_diff, raw_ignored_diff = partition_state_diff(raw_diff, ignored_state_keys)
    normalized_model = normalize_hicache_state_for_oracle_compare(model_final, page_key_mode)
    normalized_oracle = normalize_hicache_state_for_oracle_compare(oracle_final, page_key_mode)
    normalized_diff = diff_hicache_sets(normalized_model, normalized_oracle)
    active_diff, ignored_diff = partition_state_diff(normalized_diff, ignored_state_keys)

    predicted_records = load_predicted_state_records(predicted_state_trace_path)
    first_mismatch = first_hicache_mismatch(active_diff, predicted_records)
    raw_first_mismatch = first_hicache_mismatch(raw_active_diff, predicted_records)
    missing_counts = hicache_summary.get("missing_state_model_facts", {}) if hicache_summary else {}
    missing_facts = (
        sorted(str(key) for key, value in missing_counts.items() if int(value or 0) > 0)
        if isinstance(missing_counts, dict)
        else []
    )
    capacity_oracle = extract_hicache_capacity_oracle_state(snapshots)
    observed_max_counts = observed_max_derived_state_counts(snapshots)

    return {
        "state_trace_ready": bool(snapshots),
        "state_trace_events": len(snapshots),
        "oracle_state_validation_required": bool(hicache_config.get("require_oracle_state_trace", False)),
        "oracle_page_key_mode": page_key_mode,
        "model_transition_events": len(hicache_summary.get("transition_trace", []) if hicache_summary else []),
        "final_state_match": None if not oracle_final else not first_mismatch,
        "raw_final_state_match": None if not oracle_final else not raw_first_mismatch,
        "sets_diff_by_tier": active_diff,
        "raw_sets_diff_by_tier": raw_active_diff,
        "ignored_state_keys": sorted(ignored_state_keys),
        "ignored_sets_diff_by_tier": ignored_diff,
        "raw_ignored_sets_diff_by_tier": raw_ignored_diff,
        "model_final_state_counts": final_state_counts(model_final),
        "oracle_final_state_counts": final_state_counts(oracle_final),
        "normalized_model_final_state_counts": final_state_counts(normalized_model),
        "normalized_oracle_final_state_counts": final_state_counts(normalized_oracle),
        "oracle_observed_max_state_counts": observed_max_counts,
        "unchecked_model_state_keys": unchecked_model_state_keys(normalized_model, normalized_oracle),
        "first_mismatch": first_mismatch,
        "raw_first_mismatch": raw_first_mismatch,
        "request_transition_coverage": build_request_transition_coverage(predicted_records, snapshots),
        "transition_coverage": build_transition_coverage(predicted_records, snapshots),
        "event_delta_validation": build_event_delta_validation(predicted_records, snapshots),
        "timeline_delta_validation": build_timeline_delta_validation(predicted_records, snapshots),
        "oracle_capacity_summary": capacity_oracle,
        "capacity_config_audit": build_hicache_capacity_config_audit(
            capacity_oracle,
            hicache_summary.get("target_config") if isinstance(hicache_summary.get("target_config"), dict) else {},
            final_state_counts(oracle_final),
            observed_max_counts,
        ),
        "skipped_non_state_model_events": int(hicache_summary.get("skipped_non_state_model_events", 0) or 0)
        if hicache_summary
        else 0,
        "unmatched_state_trace_events": 0 if snapshots else None,
        "state_model_fact_ready": bool(hicache_summary) and not missing_facts,
        "missing_state_model_facts": missing_facts,
        "missing_state_model_fact_counts": missing_counts if isinstance(missing_counts, dict) else {},
        "oracle_trace_files": [str(path) for path in oracle_paths],
        "model_summary_ready": bool(hicache_summary),
        "predicted_state_trace_path": str(predicted_state_trace_path) if predicted_state_trace_path else None,
        "predicted_state_trace_ready": predicted_state_trace_path is not None and predicted_state_trace_path.is_file(),
    }


def configured_oracle_paths(config: dict[str, Any]) -> list[Path]:
    """Resolve configured oracle traces while rejecting non-string entries."""

    paths = config.get("oracle_trace_paths")
    if not isinstance(paths, list):
        return []
    return [require_repo_path(path) for path in paths if isinstance(path, str)]


def configured_ignore_state_keys(config: dict[str, Any]) -> set[str]:
    """Return explicitly configured state fields excluded from hard exactness."""

    raw = config.get("ignore_state_keys")
    if not isinstance(raw, list):
        return set()
    return {str(item) for item in raw if isinstance(item, str) and item}


def partition_state_diff(
    diff: dict[str, Any],
    ignored_keys: set[str],
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Partition state deltas into exactness-active and diagnostic-only maps."""

    active = {key: value for key, value in diff.items() if key not in ignored_keys}
    ignored = {key: value for key, value in diff.items() if key in ignored_keys}
    return active, ignored


def write_recommended_cpp_config(validation: dict[str, Any], output_dir: Path) -> Path | None:
    """Write a narrow C++ config when capacity evidence is complete."""

    hicache_state = validation.get("hicache_state")
    if not isinstance(hicache_state, dict):
        return None
    capacity_audit = hicache_state.get("capacity_config_audit")
    if not isinstance(capacity_audit, dict):
        return None
    recommended = capacity_audit.get("recommended_target_config")
    hicache = recommended.get("hicache") if isinstance(recommended, dict) and recommended.get("ready") else None
    if not isinstance(hicache, dict):
        return None

    output_path = output_dir / "recommended_hicache_cpp_model_config.json"
    write_json(output_path, {"hicache": {str(key): value for key, value in hicache.items() if value is not None}})
    return output_path
