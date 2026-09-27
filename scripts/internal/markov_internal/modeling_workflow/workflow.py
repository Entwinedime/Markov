"""Plan, execute and report source-only predictions in the modeling environment."""

from __future__ import annotations

from ..common.io import write_json
from ..common.paths import repo_relative_path
from .context import WorkflowOptions
from .execution.model_executor import run_model_runs
from .planning.profile_runs import discover_profile_runs
from .planning.specs import plan_model_runs
from .preflight import preflight_sources
from .prediction.hicache import build_row, read_row, summarize
from .types import ModelRunResult


def run_workflow(options: WorkflowOptions) -> int:
    """Plan, execute and report predictions; return a nonzero status for incomplete work."""

    options.output_dir.mkdir(parents=True, exist_ok=True)
    runs = discover_profile_runs(options.source_manifests)

    preflight_report = preflight_sources(options, runs)
    specs = plan_model_runs(options, runs, preflight_report)
    rows = {
        spec.run_id: build_row(ModelRunResult(spec, 0, skip_reason=spec.skip_reason or "not_started"), {})
        for spec in specs
    }
    # Publish the planned cells before starting any command. An interruption
    # leaves an incomplete current attempt, never a previous successful report.
    summary = {
        "diagnostics": options.diagnostics.value,
        "profile_run_count": len(runs),
        "model_run_count": len(specs),
        "model_run_error_count": 0,
        "preflight_ready": preflight_report["ready"],
        "prediction": summarize(list(rows.values())),
    }
    report_path = options.output_dir / "workflow_summary.json"
    write_json(report_path, summary)

    for result in run_model_runs(options, specs):
        rows[result.spec.run_id] = read_row(result)
        summary["model_run_error_count"] += result.return_code != 0
        summary["prediction"] = summarize(list(rows.values()))
        write_json(report_path, summary)

    prediction_summary = summary["prediction"]
    print(
        f"prediction={prediction_summary['status']} "
        f"completed={prediction_summary['completed_count']}/{len(specs)} "
        f"cost-coverage={prediction_summary['cost_coverage_counts']}",
        flush=True,
    )
    print("Predicted HTTP time (not measured target time):")
    for row in rows.values():
        duration = row["http_e2e_us"]
        detail = f"{duration / 1_000_000:.6f} s" if duration is not None else "not predicted"
        reasons = [need["component"] for need in row["missing_costs"]] or row["blockers"]
        if reasons:
            detail += " | " + ", ".join(reasons)
        print(f"  {row['source_config_id']} -> {row['target_config']} | {row['workload_id']} | {detail}")
    print(
        f"Costs may be extrapolated and retain base waits; inspect coverage and approximations in {repo_relative_path(report_path)}"
    )
    return 0 if options.dry_run or prediction_summary["status"] == "EXECUTED" else 2
