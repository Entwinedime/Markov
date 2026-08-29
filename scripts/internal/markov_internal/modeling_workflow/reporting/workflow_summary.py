"""Writer for the compact top-level workflow summary."""

from __future__ import annotations

from typing import Any

from ...common.io import write_json
from ..context import WorkflowContext
from ..types import ModelRunCounts, ModelRunResult, ModelRunSpec


def write_workflow_summary(
    context: WorkflowContext,
    *,
    specs: list[ModelRunSpec],
    preflight_report: dict[str, object],
    results: dict[str, ModelRunResult],
    prediction_summary: dict[str, Any],
) -> dict[str, Any]:
    """Persist aggregate execution and validation outcomes."""

    counts = ModelRunCounts.from_results(list(results.values()))
    payload = {
        "diagnostics": context.options.artifact_policy.diagnostics.value,
        "profile_run_count": len(context.runs),
        "model_run_count": len(specs),
        "model_run_handled_count": counts.handled,
        "model_run_runnable_count": counts.runnable,
        "model_run_usable_count": counts.usable,
        "model_run_error_count": counts.errors,
        "model_run_skipped_count": counts.skipped,
        "preflight_ready": preflight_report.get("ready"),
        "prediction": prediction_summary,
    }
    write_json(context.artifacts.workflow_summary_path, payload)
    return payload
