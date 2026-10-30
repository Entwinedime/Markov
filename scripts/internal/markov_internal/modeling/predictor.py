"""Plan, execute and report source-only predictions in the modeling environment."""

from __future__ import annotations

import time
from dataclasses import replace
from pathlib import Path
from typing import Any

from markov_internal.common.io import write_json, write_json_atomic
from markov_internal.common.paths import repo_relative_path
from markov_internal.common.process import container_peak_memory_bytes
from markov_internal.modeling.context import WorkflowOptions
from markov_internal.modeling.execution.model_executor import ExecutionSession, plan_model_runs, run_model_runs
from markov_internal.modeling.source_dag.profiles import ProfileRunRef, discover_profile_runs
from markov_internal.modeling.preflight import preflight_sources
from markov_internal.modeling.execution.results import build_row, read_row, summarize
from markov_internal.modeling.types import ModelRunResult, ModelRunStarted, SourcePreparation, TargetHiCacheConfig
from markov_internal.modeling.deployment import cost_environment_matches, source_environment


class PredictionSession:
    """An explicit fixed-input lifetime, with at most one resident native source."""

    def __init__(self, options: WorkflowOptions) -> None:
        self.options = options
        self.runs = discover_profile_runs(options.source_manifests)
        self.execution = ExecutionSession(options.output_dir, options.diagnostics)
        self.preflight: dict[str, Any] | None = None
        costs = options.cost_models
        # Isolation follows actual capture lineage, not suite-local run labels.
        if set(costs.source_manifests) & {source.manifest_path.resolve() for source in self.runs}:
            raise ValueError("current base captures must be excluded from independently trained prediction costs")
        if any(
            not cost_environment_matches(source_environment(source, options.source_resources), costs.deployment)
            for source in self.runs
        ):
            raise ValueError("database cost model does not apply to the selected source deployment")

    def __enter__(self) -> PredictionSession:
        return self

    def __exit__(self, *exc: Any) -> None:
        self.execution.close()

    def predict(self, targets: tuple[TargetHiCacheConfig, ...], output_dir: Path) -> dict[str, Any]:
        started = time.monotonic()
        options = replace(self.options, target_configs=targets, output_dir=output_dir)
        options.output_dir.mkdir(parents=True, exist_ok=True)
        (options.output_dir / "workflow_summary.json").unlink(missing_ok=True)
        reused = self.preflight is not None
        if self.preflight is None:
            self.preflight = preflight_sources(self.options, self.runs)
        write_json(options.output_dir / "preflight_summary.json", self.preflight)
        return _predict(options, self.runs, self.preflight, self.execution, started, reused)


def predict(options: WorkflowOptions) -> dict[str, Any]:
    """One-shot public call through the same session used for later target batches."""

    started = time.monotonic()
    with PredictionSession(options) as session:
        summary = session.predict(options.target_configs, options.output_dir)
        close_started = time.monotonic()
    summary["performance"]["session_close_wall_seconds"] = time.monotonic() - close_started
    summary["performance"]["one_shot_wall_seconds"] = time.monotonic() - started
    write_json_atomic(options.output_dir / "workflow_summary.json", summary)
    return summary


def _predict(
    options: WorkflowOptions,
    runs: list[ProfileRunRef],
    preflight_report: dict[str, Any],
    execution: ExecutionSession,
    started: float,
    reused: bool,
) -> dict[str, Any]:
    report_path = options.output_dir / "workflow_summary.json"
    costs = options.cost_models
    specs = plan_model_runs(options, runs, preflight_report)
    rows = {
        spec.run_id: {
            **build_row(ModelRunResult(spec, 0, skip_reason=spec.skip_reason or "not_started"), {}),
            "attempted": False,
        }
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
        "cost_training_captures": list(costs.source_captures),
        "cost_training_manifests": [str(repo_relative_path(path)) for path in costs.source_manifests],
        "source_preparations": [],
        "source_reuses": [],
        "prediction": summarize(list(rows.values())),
    }
    write_json_atomic(report_path, summary)

    for result in run_model_runs(options, specs, execution):
        if isinstance(result, SourcePreparation):
            summary["source_reuses" if result.reused else "source_preparations"].append(
                {
                    "source_manifest": str(repo_relative_path(result.manifest)),
                    "cpu_service_cost": str(repo_relative_path(result.cpu_service_cost))
                    if result.cpu_service_cost
                    else None,
                    "wall_seconds": result.wall_seconds,
                    "performance": result.performance,
                }
            )
        elif isinstance(result, ModelRunStarted):
            rows[result.spec.run_id]["attempted"] = True
        else:
            attempted = rows[result.spec.run_id]["attempted"]
            rows[result.spec.run_id] = {**read_row(result), "attempted": attempted}
            summary["model_run_error_count"] += result.return_code != 0

        summary["prediction"] = summarize(list(rows.values()))
        write_json_atomic(report_path, summary)

    summary["performance"] = {
        "prediction_wall_seconds": time.monotonic() - started,
        "preflight_reused": reused,
        "scope": "current batch inside an open session, including any new preflight/source preparation, targets and reports; excludes capture/training and final session close",
    }
    peak = container_peak_memory_bytes()
    if peak is not None:
        summary["performance"]["container_peak_memory_bytes"] = peak

    write_json_atomic(report_path, summary)
    return summary
