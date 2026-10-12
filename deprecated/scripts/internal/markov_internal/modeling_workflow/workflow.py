"""Plan, execute and report source-only predictions in the modeling environment."""

from __future__ import annotations

from ..common.io import write_json
from ..common.paths import repo_relative_path
from .context import WorkflowOptions
from .execution.model_executor import run_model_runs
from ..modeling.source_dag.profiles import discover_profile_runs
from .structure.specs import plan_model_runs
from .preflight import preflight_sources
from .execution.results import build_row, read_row, summarize
from .types import ModelRunResult
from ..modeling.deployment import environments_match, require_cost_environment, source_environment


def run_workflow(options: WorkflowOptions) -> int:
    """Plan, execute and report predictions; return a nonzero status for incomplete work."""

    options.output_dir.mkdir(parents=True, exist_ok=True)
    runs = discover_profile_runs(options.source_manifests)
    if set(options.cost_sources) & {source.run_id for source in runs} or set(options.cost_source_manifests) & {
        source.manifest_path.resolve() for source in runs
    }:
        raise ValueError("current base captures must be excluded from independently trained prediction costs")
    if options.cost_deployment is not None:
        require_cost_environment(options.cost_deployment)
        if any(
            not environments_match(source_environment(source, options.source_resources), options.cost_deployment)
            for source in runs
        ):
            raise ValueError("database cost model does not apply to the selected source deployment")

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
        "experiment_reports": options.experiment_reports,
        "profile_run_count": len(runs),
        "model_run_count": len(specs),
        "model_run_error_count": 0,
        "preflight_ready": preflight_report["ready"],
        "cost_training_captures": list(options.cost_sources),
        "cost_training_manifests": [str(repo_relative_path(path)) for path in options.cost_source_manifests],
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
        reasons = (
            [gap["reason"] for gap in row["structure_gaps"]]
            or [need["component"] for need in row["missing_costs"]]
            or row["blockers"]
        )
        if reasons:
            detail += " | " + ", ".join(reasons)
        print(f"  {row['source_config_id']} -> {row['target_config']} | {row['workload_id']} | {detail}")
    print(
        f"Costs may be extrapolated and retain base waits; inspect coverage and approximations in {repo_relative_path(report_path)}"
    )
    return 0 if options.dry_run or prediction_summary["status"] == "EXECUTED" else 2
