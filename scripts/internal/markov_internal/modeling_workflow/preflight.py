"""Source readiness shared by prediction planning and the user-facing report."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from ..common.io import write_json
from .progress import StageProgress, count_text
from .types import ProfileRunRef
from .validations.hicache.preflight.profile import audit_hicache_profile

if TYPE_CHECKING:
    from .context import WorkflowOptions


def preflight_sources(options: WorkflowOptions, runs: list[ProfileRunRef]) -> dict[str, Any]:
    """Audit each source once; retain compact blockers even without diagnostics."""

    progress = StageProgress("preflight", len(runs), "Source traces and HiCache facts", unit="source")
    retain_details = options.diagnostics.keep_debug_artifacts
    sources: list[dict[str, Any]] = []
    for index, run in enumerate(runs, start=1):
        audit = audit_hicache_profile(run.manifest_path)
        audit_path = options.output_dir / "artifacts" / "preflight" / f"source_{index}.profile_audit.json"
        if retain_details:
            write_json(audit_path, audit)
        coverage = audit["trace_channel_coverage"]
        errors = audit["artifact_errors"]
        full_trace_ready = (
            all(coverage[f"{channel}_trace_files"] > 0 for channel in ("torch", "ld_preload", "python_probe"))
            and "trace_channel_missing" not in errors
            and "sidecar_only_trace" not in errors
        )
        skip_reason = ""
        if not full_trace_ready:
            skip_reason = "full_dag_trace_not_ready"
            if errors:
                skip_reason += ":" + ",".join(errors)
        elif not audit["workflow_input_ready"]:
            skip_reason = "source_workflow_input_not_ready"
        sources.append(
            {
                "run_id": run.run_id,
                "config_id": run.config_id,
                "input_id": run.input_id,
                "manifest_path": str(run.manifest_path),
                "audit_path": str(audit_path) if retain_details else None,
                "full_trace_ready": full_trace_ready,
                "workflow_input_ready": audit["workflow_input_ready"],
                "skip_reason": skip_reason,
                "missing_trace_channels": audit["missing_trace_channels"],
                "artifact_errors": errors,
                "workflow_input_errors": audit["workflow_input_errors"],
            }
        )
        progress.advance({"ready": count_text(int(full_trace_ready and audit["workflow_input_ready"]), 1)})

    run_count = len(runs)
    dag_ready = sum(row["full_trace_ready"] for row in sources)
    state_ready = sum(row["workflow_input_ready"] for row in sources)
    report = {
        "run_count": run_count,
        "ready": run_count > 0 and dag_ready == state_ready == run_count,
        "full_trace_ready_count": dag_ready,
        "workflow_input_ready_count": state_ready,
        "sources": sources,
    }
    write_json(options.output_dir / "preflight_summary.json", report)
    detail = f"full-dag {count_text(dag_ready, run_count)} | hicache {count_text(state_ready, run_count)}"
    progress.finish("OK" if report["ready"] else "CHECK", detail)
    return report
