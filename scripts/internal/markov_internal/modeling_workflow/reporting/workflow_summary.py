"""Writer for the compact top-level workflow summary."""

from __future__ import annotations

from typing import Any

from ...common.io import write_json
from ..context import WorkflowContext
from ..types import ModelRunCounts, ModelRunResult, ModelRunSpec, ValidationSummary


def _validation_payload(summary: ValidationSummary) -> dict[str, Any]:
    """Project one validation result into the compact workflow contract."""

    payload = {
        "status": summary.status,
        "selected_run_count": summary.selected_run_count,
        "ready_count": summary.ready_count,
        "exact_count": summary.exact_count,
        "skipped_count": summary.skipped_count,
        "blocker_counts": summary.blocker_counts,
        "artifact_paths": summary.artifact_paths,
    }
    closure_fields = (
        "closure_review_ready",
        "closure_classification_counts",
        "closure_unrelated_semantic_mismatch_count",
        "closure_not_ready_count",
    )
    for field in closure_fields:
        if field in summary.payload:
            payload[field] = summary.payload[field]
    return payload


def write_workflow_summary(
    context: WorkflowContext,
    *,
    specs: list[ModelRunSpec],
    preflight_report: dict[str, object],
    results: dict[str, ModelRunResult],
    validation_summaries: list[ValidationSummary],
) -> dict[str, Any]:
    """Persist aggregate execution and validation outcomes."""

    counts = ModelRunCounts.from_results(list(results.values()))
    payload = {
        "schema": "trace_sim.modeling_workflow.summary.v1",
        "selected_validations": list(context.options.validations),
        "profile_run_count": len(context.runs),
        "model_run_count": len(specs),
        "model_run_handled_count": counts.handled,
        "model_run_runnable_count": counts.runnable,
        "model_run_usable_count": counts.usable,
        "model_run_error_count": counts.errors,
        "model_run_skipped_count": counts.skipped,
        "model_run_reused_count": counts.reused,
        "preflight_ready": preflight_report.get("ready"),
        "hicache_io_model": context.options.hicache_io_model.metadata()
        if context.options.hicache_io_model is not None
        else None,
        "validations": {summary.name: _validation_payload(summary) for summary in validation_summaries},
    }
    write_json(context.artifacts.workflow_summary_path, payload)
    return payload
