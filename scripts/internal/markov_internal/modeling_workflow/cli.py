"""Container-side command-line interface for the modeling workflow."""

from __future__ import annotations

import argparse
from pathlib import Path

from ..common.commands import positive_int
from ..common.io import load_json
from ..common.paths import require_repo_path
from ..modeling.cost.hicache import HiCacheIoModel
from ..modeling.deployment import require_cost_environment
from ..modeling.source_dag.profiles import cpu_service_inputs
from .context import DiagnosticLevel, WorkflowOptions
from .types import TargetHiCacheConfig
from .workflow import run_workflow


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
        help="Maximum model-run subprocesses to execute concurrently.",
    )
    args = parser.parse_args(argv)
    trained = load_json(require_repo_path(args.cost_model))
    require_cost_environment(trained["deployment"])
    sources = tuple(require_repo_path(path).resolve() for path in args.source_manifest)
    output = (
        require_repo_path(args.output_dir)
        if args.output_dir
        else require_repo_path(args.source_manifest[0]).parent / "modeling" / "hicache_prediction"
    )
    numerical = {**trained["hicache"], "operator_models": trained["operator_models"]}
    io_model = HiCacheIoModel.from_raw(numerical)
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
        hicache_io_model=io_model,
        cost_deployment=trained["deployment"],
        cost_sources=tuple(trained["training_sources"]),
        cost_source_manifests=tuple(require_repo_path(path).resolve() for path in trained["training_source_manifests"]),
        source_resources=load_json(require_repo_path(args.resource_environment)) if args.resource_environment else None,
        experiment_reports=args.experiment_reports,
    )
    return run_workflow(options)


if __name__ == "__main__":
    raise SystemExit(main())
