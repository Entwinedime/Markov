"""workflow 顶层 summary 写出器。"""

from __future__ import annotations

from typing import Any

from ...common.io import write_json
from ..types import ModelRunResult, ModelRunSpec, ProfileRunRef, ValidationSummary


def write_workflow_summary(
    context: Any,
    *,
    runs: list[ProfileRunRef],
    specs: list[ModelRunSpec],
    preflight_report: dict[str, Any],
    results: dict[str, ModelRunResult],
    validation_summaries: list[ValidationSummary],
) -> dict[str, Any]:
    """写出统一 workflow 的最终 summary。"""

    payload = {
        "schema": "trace_sim.modeling_workflow.summary.v1",
        "selected_validations": list(context.options.validations),
        "profile_run_count": len(runs),
        "model_run_count": len(specs),
        "model_run_handled_count": len(results),
        "model_run_runnable_count": sum(1 for result in results.values() if not result.skipped),
        "model_run_usable_count": sum(
            1 for result in results.values() if not result.skipped and result.return_code == 0
        ),
        "model_run_error_count": sum(
            1 for result in results.values() if not result.skipped and result.return_code not in (0, None)
        ),
        "model_run_skipped_count": sum(1 for result in results.values() if result.skipped),
        "preflight_ready": preflight_report.get("ready"),
        "validations": {
            summary.name: {
                "status": summary.status,
                "selected_run_count": summary.selected_run_count,
                "ready_count": summary.ready_count,
                "exact_count": summary.exact_count,
                "skipped_count": summary.skipped_count,
                "blocker_counts": summary.blocker_counts,
                "artifact_paths": summary.artifact_paths,
            }
            for summary in validation_summaries
        },
    }
    write_json(context.artifacts.workflow_summary_path, payload)
    return payload
