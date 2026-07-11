"""Generation of diagnostic HiCache transition operation-gate artifacts.

This module only orchestrates loading, assembly, and persistence. Model-side
gates, observed-side gates, and coverage checks remain in focused modules.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from markov_internal.common.io import load_json, write_json
from markov_internal.common.paths import resolve_repo_path

from ..replay.record_schema import load_predicted_trace, predicted_records
from .operation_gate_coverage import (
    build_transition_patch_gate_coverage,
    count_gates_by_field,
    count_gates_by_kind,
    operation_gate_schema_ready,
    summarize_gate_rows_by_key,
)
from .operation_gate_model import build_model_operation_gates
from .operation_gate_observed import build_observed_operation_gates
from .taxonomy_evidence import load_hicache_summary


def build_transition_patch_gate_scoreboard_from_entries(
    artifact_root: Path,
    prediction_entries: list[dict[str, Any]],
    *,
    page_key_mode: str,
    sample_limit: int,
) -> dict[str, Any]:
    """Build a patch-gate scoreboard from comparison classifications."""

    rows: list[dict[str, Any]] = []
    skipped: list[dict[str, Any]] = []
    for prediction_entry in prediction_entries:
        paths = prediction_gate_input_paths(prediction_entry)
        if paths.get("skip_reason"):
            skipped.append(serializable_gate_skip(paths))
            continue
        rows.append(
            write_prediction_gate_outputs(
                paths["prediction_dir"],
                paths["observed_path"],
                prediction_entry,
                page_key_mode=page_key_mode,
                sample_limit=sample_limit,
            )
        )
    return {
        "schema": "trace_sim.hicache.transition_patch_gate_scoreboard.v1",
        "artifact_root": str(artifact_root),
        "prediction_count": len(rows),
        "skipped_prediction_count": len(skipped),
        "operation_gate_schema_ready_count": sum(1 for row in rows if row.get("operation_gate_schema_ready")),
        "transition_coverage_ready_count": sum(1 for row in rows if row.get("transition_coverage_ready")),
        "state_marker_filter_ready_count": sum(1 for row in rows if row.get("state_marker_filter_ready")),
        "unresolved_report_ready_count": sum(1 for row in rows if row.get("unresolved_report_ready")),
        "ready": bool(rows)
        and not skipped
        and all(row.get("operation_gate_schema_ready") for row in rows)
        and all(row.get("transition_coverage_ready") for row in rows)
        and all(row.get("state_marker_filter_ready") for row in rows)
        and all(row.get("unresolved_report_ready") for row in rows),
        "patch_allowed": False,
        "by_family": summarize_gate_rows_by_key(rows, "transition_family"),
        "by_target_config": summarize_gate_rows_by_key(rows, "target_config_id"),
        "predictions": rows,
        "skipped_predictions": skipped[:sample_limit],
        "notes": [
            "This artifact contains diagnostic operation gates only; it does not emit patch actions.",
            "Readiness fields only validate gate coverage and filtering boundaries, not source attribution or DAG patch readiness.",
        ],
    }


def prediction_gate_input_paths(prediction_entry: dict[str, Any]) -> dict[str, Any]:
    """Resolve gate inputs or return a serializable skip reason."""

    prediction_dir_raw = str(prediction_entry.get("prediction_dir") or "")
    observed_path_raw = str(prediction_entry.get("observed_target_trace_path") or "")
    prediction_dir = resolve_repo_path(Path(prediction_dir_raw)) if prediction_dir_raw else None
    observed_path = resolve_repo_path(Path(observed_path_raw)) if observed_path_raw else None
    base = {
        "label": prediction_entry.get("label"),
        "input_id": prediction_entry.get("input_id"),
        "source_config_id": prediction_entry.get("source_config_id"),
        "target_config_id": prediction_entry.get("target_config_id"),
        "prediction_dir": prediction_dir,
        "observed_path": observed_path,
    }
    if not prediction_dir_raw:
        return {**base, "skip_reason": "missing_prediction_dir"}
    if not observed_path_raw:
        return {**base, "skip_reason": "missing_observed_target_trace_path"}
    predicted_trace = prediction_dir / "predicted_target_cache_state_trace.json"
    if not predicted_trace.is_file():
        return {**base, "skip_reason": "missing_predicted_trace", "predicted_trace": str(predicted_trace)}
    if observed_path is None or not observed_path.is_file():
        return {**base, "skip_reason": "missing_observed_target_trace"}
    return base


def serializable_gate_skip(paths: dict[str, Any]) -> dict[str, Any]:
    """Convert a gate-skip row to JSON-compatible path strings."""

    result = dict(paths)
    for key in ("prediction_dir", "observed_path"):
        if isinstance(result.get(key), Path):
            result[key] = str(result[key])
    return result


def write_prediction_gate_outputs(
    prediction_dir: Path,
    observed_path: Path,
    prediction_entry: dict[str, Any],
    *,
    page_key_mode: str,
    sample_limit: int,
) -> dict[str, Any]:
    """Write operation-gate artifacts and return one scoreboard row."""

    model_payload, observed_payload, coverage, row = build_prediction_gate_artifacts(
        prediction_dir,
        observed_path,
        prediction_entry,
        page_key_mode=page_key_mode,
        sample_limit=sample_limit,
    )
    write_json(prediction_dir / "transition_operation_gate_model.json", model_payload)
    write_json(prediction_dir / "transition_operation_gate_observed.json", observed_payload)
    write_json(prediction_dir / "transition_patch_gate_coverage.json", coverage)
    return row


def build_prediction_gate_artifacts(
    prediction_dir: Path,
    observed_path: Path,
    prediction_entry: dict[str, Any],
    *,
    page_key_mode: str,
    sample_limit: int,
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any], dict[str, Any]]:
    """Build model/observed gate payloads, coverage, and scoreboard row."""

    predicted = load_predicted_trace(prediction_dir / "predicted_target_cache_state_trace.json")
    observed = load_json(observed_path) if observed_path.is_file() else {}
    hicache_summary = load_hicache_summary(prediction_dir / "model_summary.json")
    records = predicted_records(predicted)
    model_gates = build_model_operation_gates(
        records,
        hicache_summary,
        prediction_entry,
        page_key_mode=page_key_mode,
    )
    observed_gates, observed_filter = build_observed_operation_gates(
        observed, prediction_entry, page_key_mode=page_key_mode, sample_limit=sample_limit
    )
    coverage = build_transition_patch_gate_coverage(records, model_gates, sample_limit=sample_limit)
    model_payload = build_model_gate_payload(prediction_dir, prediction_entry, model_gates)
    observed_payload = build_observed_gate_payload(
        prediction_dir, observed_path, prediction_entry, observed, observed_gates, observed_filter
    )
    row = build_gate_scoreboard_row(prediction_dir, prediction_entry, model_gates, observed_gates, coverage)
    return model_payload, observed_payload, coverage, row


def build_model_gate_payload(
    prediction_dir: Path, prediction_entry: dict[str, Any], model_gates: list[dict[str, Any]]
) -> dict[str, Any]:
    """Assemble the model-side operation-gate JSON payload."""

    return {
        "schema": "trace_sim.hicache.transition_operation_gate_model.v1",
        "gate_maturity": "diagnostic",
        "patch_allowed": False,
        "prediction_dir": str(prediction_dir),
        "transition_family": prediction_entry.get("family"),
        "operation_gate_count": len(model_gates),
        "operation_gate_count_by_kind": count_gates_by_kind(model_gates),
        "operation_gate_count_by_classification": count_gates_by_field(model_gates, "classification"),
        "operation_gates": model_gates,
        "notes": [
            "Operation gates are used only for transition mismatch classification, coverage, and downstream DAG patch filtering.",
            "patch_allowed is fixed to false in this diagnostic stage.",
        ],
    }


def build_observed_gate_payload(
    prediction_dir: Path,
    observed_path: Path,
    prediction_entry: dict[str, Any],
    observed: dict[str, Any],
    observed_gates: list[dict[str, Any]],
    observed_filter: dict[str, Any],
) -> dict[str, Any]:
    """Assemble the observed-side operation-gate JSON payload."""

    return {
        "schema": "trace_sim.hicache.transition_operation_gate_observed.v1",
        "gate_maturity": "diagnostic",
        "patch_allowed": False,
        "prediction_dir": str(prediction_dir),
        "observed_target_trace_path": str(observed_path),
        "transition_family": prediction_entry.get("family"),
        "oracle_ready": bool(observed.get("oracle_ready")),
        "operation_gate_count": len(observed_gates),
        "operation_gate_count_by_kind": count_gates_by_kind(observed_gates),
        "filter_summary": observed_filter,
        "operation_gates": observed_gates,
        "notes": [
            "Observed operation gates are validation-only evidence.",
            "source_actual and timing_observation are not fed back into the normal state model.",
        ],
    }


def build_gate_scoreboard_row(
    prediction_dir: Path,
    prediction_entry: dict[str, Any],
    model_gates: list[dict[str, Any]],
    observed_gates: list[dict[str, Any]],
    coverage: dict[str, Any],
) -> dict[str, Any]:
    """Assemble one prediction's gate-scoreboard row."""

    return {
        "label": prediction_entry.get("label"),
        "input_id": prediction_entry.get("input_id"),
        "source_config_id": prediction_entry.get("source_config_id"),
        "target_config_id": prediction_entry.get("target_config_id"),
        "prediction_dir": str(prediction_dir),
        "transition_family": prediction_entry.get("family"),
        "classification": prediction_entry.get("classification"),
        "patch_risk": prediction_entry.get("patch_risk"),
        "patch_filter_action": prediction_entry.get("patch_filter_action"),
        "source_attribution_required": prediction_entry.get("source_attribution_required"),
        "duration_required": prediction_entry.get("duration_required"),
        "evidence_required": prediction_entry.get("evidence_required"),
        "operation_gate_schema_ready": operation_gate_schema_ready(model_gates),
        "transition_coverage_ready": bool(coverage.get("coverage_ready")),
        "state_marker_filter_ready": bool(coverage.get("state_marker_filter_ready")),
        "unresolved_report_ready": bool(coverage.get("unresolved_report_ready")),
        "transition_count": coverage.get("transition_count"),
        "covered_transition_count": coverage.get("covered_transition_count"),
        "unresolved_transition_count": coverage.get("unresolved_transition_count"),
        "model_operation_gate_count": len(model_gates),
        "observed_operation_gate_count": len(observed_gates),
        "model_operation_gate_count_by_kind": count_gates_by_kind(model_gates),
        "observed_operation_gate_count_by_kind": count_gates_by_kind(observed_gates),
    }
