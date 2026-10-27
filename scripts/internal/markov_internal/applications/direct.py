"""Container-side command-line interface for the modeling workflow."""

from __future__ import annotations

import argparse
from pathlib import Path

from markov_internal.common.commands import positive_int
from markov_internal.common.io import load_json, write_json_atomic
from markov_internal.common.paths import repo_relative_path, require_repo_path
from markov_internal.modeling.cost.hicache import HiCacheCostModels
from markov_internal.modeling.source_dag.profiles import cpu_service_inputs
from markov_internal.modeling.context import DiagnosticLevel, WorkflowOptions
from markov_internal.modeling.types import TargetHiCacheConfig
from markov_internal.modeling.predictor import predict
from markov_internal.applications.comparison import configuration_comparison


def display_prediction(summary: dict, report_path: Path) -> None:
    """Show model results without changing their values or readiness."""

    prediction_summary = summary["prediction"]
    print(
        f"prediction={prediction_summary['status']} "
        f"completed={prediction_summary['completed_count']}/{summary['model_run_count']} "
        f"cost-coverage={prediction_summary['cost_coverage_counts']}",
        flush=True,
    )
    print("Predicted HTTP time (not measured target time):")
    for row in prediction_summary["cells"]:
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

        if duration is None:
            print(f"    {row['next_action']}")
            if row["failure_log"] is not None:
                print(f"    log: {row['failure_log']}")

    print("Configuration comparison (prediction only, not a validated recommendation):")
    for comparison in prediction_summary["configuration_comparison"]:
        label = f"{comparison['source_config_id']} | {comparison['workload_id']}"
        print(f"  {label} | {comparison['status']}")
        for candidate in comparison["ordered_targets"]:
            gain = candidate["time_reduction_vs_base"]
            relative = f" | {gain:+.3%} time reduction vs predicted base" if gain is not None else ""
            print(f"    {candidate['target_config']}: {candidate['http_e2e_us'] / 1_000_000:.6f} s{relative}")

        if comparison["unpredicted_targets"]:
            print("    not predicted: " + ", ".join(comparison["unpredicted_targets"]))

    print(
        f"Costs may be extrapolated and retain base waits; inspect coverage and approximations in {repo_relative_path(report_path)}"
    )


def main(argv: list[str] | None = None) -> int:
    """Normalize public inputs once, then run the modeling workflow."""

    parser = argparse.ArgumentParser(
        description="Predict HiCache I/O/control and Prefill/Decode changes from one profiled base."
    )
    inputs = parser.add_argument_group("prediction inputs")
    inputs.add_argument(
        "--source-manifest",
        type=Path,
        action="append",
        required=True,
        help="Source profile_manifest.json. Can be repeated for multiple workloads.",
    )
    inputs.add_argument(
        "--target-config",
        type=Path,
        action="append",
        required=True,
        help="Explicit {name?, hicache} target config. Can be repeated.",
    )
    inputs.add_argument(
        "--output-dir",
        type=Path,
        help="Output directory. Defaults beside the first source profile.",
    )

    model_inputs = parser.add_argument_group("model inputs")
    model_inputs.add_argument(
        "--cpu-service-cost",
        type=Path,
        action="append",
        default=[],
        help="Same-base CPU service intervals; repeat for distinct source manifests.",
    )
    model_inputs.add_argument(
        "--cost-model",
        type=Path,
        required=True,
        help="Offline-trained database cost package; the current base is not fitted online.",
    )
    model_inputs.add_argument(
        "--resource-environment",
        type=Path,
        help="explicit resource declaration for historical bases lacking recorded hardware/runtime metadata",
    )

    artifacts = parser.add_argument_group("artifacts and diagnostics")
    artifacts.add_argument(
        "--experiment-reports",
        action="store_true",
        help="Retain source replay and executed structure evidence for independent evaluation",
    )
    artifacts.add_argument(
        "--diagnostics",
        default=DiagnosticLevel.OFF.value,
        choices=tuple(level.value for level in DiagnosticLevel),
        help=("Optional diagnostics: full retains per-cell rows, C++ details, and successful model logs."),
    )

    execution = parser.add_argument_group("execution")
    execution.add_argument("--trace-threads", type=positive_int, default=1, help="C++ logical trace read/build budget.")
    execution.add_argument("--trace-file-threads", type=positive_int, default=1, help="C++ per-file parse threads.")
    execution.add_argument("--dry-run", action="store_true", help="Write the execution plan without running C++.")
    execution.add_argument("--continue-on-error", action="store_true", help="Continue after a model command fails.")
    execution.add_argument(
        "--model-run-jobs",
        type=positive_int,
        default=1,
        help="Maximum concurrent target executions per prepared source; source groups run in sequence.",
    )
    args = parser.parse_args(argv)
    cost_models = HiCacheCostModels.from_raw(load_json(require_repo_path(args.cost_model)))
    sources = tuple(require_repo_path(path).resolve() for path in args.source_manifest)
    output = (
        require_repo_path(args.output_dir)
        if args.output_dir
        else require_repo_path(args.source_manifest[0]).parent / "modeling" / "hicache_prediction"
    )
    targets = tuple(TargetHiCacheConfig.load(path) for path in args.target_config)
    if len({target.label for target in targets}) != len(targets):
        raise ValueError("target config names must be unique within one prediction request")

    options = WorkflowOptions(
        source_manifests=sources,
        cpu_service_costs=cpu_service_inputs(args.cpu_service_cost, sources),
        target_configs=targets,
        output_dir=output,
        diagnostics=DiagnosticLevel(args.diagnostics),
        dry_run=args.dry_run,
        continue_on_error=args.continue_on_error,
        trace_threads=args.trace_threads,
        trace_file_threads=args.trace_file_threads,
        model_run_jobs=args.model_run_jobs,
        cost_models=cost_models,
        source_resources=load_json(require_repo_path(args.resource_environment)) if args.resource_environment else None,
        experiment_reports=args.experiment_reports,
    )
    try:
        summary = predict(options)
        prediction = summary["prediction"]
        prediction["configuration_comparison"] = configuration_comparison(prediction["cells"])
        report = output / "workflow_summary.json"
        write_json_atomic(report, summary)
        display_prediction(summary, report)
        return 0 if options.dry_run or prediction["status"] == "EXECUTED" else 2
    except KeyboardInterrupt:
        print("Prediction interrupted; incomplete targets have no valid prediction time.", flush=True)
        print(f"output_dir={repo_relative_path(output)}", flush=True)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
