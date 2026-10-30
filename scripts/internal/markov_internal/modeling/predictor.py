"""Plan, execute and report source-only predictions in the modeling environment."""

from __future__ import annotations

import time
from pathlib import Path
from typing import Any

from markov_internal.common.io import write_json_atomic
from markov_internal.common.paths import repo_relative_path
from markov_internal.modeling.context import WorkflowOptions
from markov_internal.modeling.execution.model_executor import plan_model_runs, run_model_runs
from markov_internal.modeling.source_dag.profiles import discover_profile_runs
from markov_internal.modeling.preflight import preflight_sources
from markov_internal.modeling.execution.results import build_row, read_row, summarize
from markov_internal.modeling.types import ModelRunResult, ModelRunStarted, SourcePreparation
from markov_internal.modeling.deployment import cost_environment_matches, source_environment


def predict(options: WorkflowOptions) -> dict[str, Any]:
    """Return compact execution facts; HTTP times are microseconds or None on failure.

    Inputs are already prepared and read-only. This call does not capture, train,
    rank candidates or read target observations. Current reports are published
    after each completion so interruption never exposes an old successful run.
    """

    started = time.monotonic()
    options.output_dir.mkdir(parents=True, exist_ok=True)
    report_path = options.output_dir / "workflow_summary.json"
    report_path.unlink(missing_ok=True)
    runs = discover_profile_runs(options.source_manifests)
    costs = options.cost_models
    # Suite-local run labels are not database identities. Complete manifest
    # provenance carries original captures, known aliases and paired inputs.
    if set(costs.source_manifests) & {source.manifest_path.resolve() for source in runs}:
        raise ValueError("current base captures must be excluded from independently trained prediction costs")
    if any(
        not cost_environment_matches(source_environment(source, options.source_resources), costs.deployment)
        for source in runs
    ):
        raise ValueError("database cost model does not apply to the selected source deployment")

    preflight_report = preflight_sources(options, runs)
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
        "prediction": summarize(list(rows.values())),
    }
    write_json_atomic(report_path, summary)

    for result in run_model_runs(options, specs):
        if isinstance(result, SourcePreparation):
            summary["source_preparations"].append(
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
        "scope": "prepared-input prediction including preflight, source preparation, targets and incremental reports; excludes capture/training",
    }
    # Modeling Docker mounts its own controller at these roots, including v1
    # hosts whose /proc membership retains the full host-side scope path.
    for path in (Path("/sys/fs/cgroup/memory.peak"), Path("/sys/fs/cgroup/memory/memory.max_usage_in_bytes")):
        if path.is_file():
            summary["performance"]["container_peak_memory_bytes"] = int(path.read_text())
            break

    write_json_atomic(report_path, summary)
    return summary
