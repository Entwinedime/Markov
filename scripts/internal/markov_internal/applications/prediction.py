"""Host application: prepare one base, then call the shared predictor."""

from __future__ import annotations

import argparse
from pathlib import Path
import time
from typing import Any

from markov_internal.common.commands import positive_int
from markov_internal.common.io import load_json, write_json
from markov_internal.common.manifest import profile_labels
from markov_internal.common.paths import repo_relative_path, require_repo_path, running_in_modeling_container
from markov_internal.modeling.context import DiagnosticLevel, PredictionInputs
from markov_internal.modeling.deployment import source_environment
from markov_internal.modeling.invoke import invoke_prediction
from markov_internal.modeling.source_dag.profiles import extract_hicache_from_server_command
from markov_internal.modeling.types import TargetHiCacheConfig
from markov_internal.preparation.base import prepare_inputs
from markov_internal.preparation.capture import pending_group_capture
from markov_internal.preparation.scenario import ScenarioRequest
from markov_internal.profiling.suite import profile_experiments


def load_targets(scenario: ScenarioRequest) -> tuple[TargetHiCacheConfig, ...]:
    """Resolve the public group's target values, paths or declared suite labels."""

    values = load_json(scenario.path)["target_configs"]
    servers: dict[str, list[list[str]]] = {}
    if scenario.profile_suite is not None:
        for experiment in profile_experiments(load_json(scenario.profile_suite)):
            config, _ = profile_labels(experiment)
            servers.setdefault(config, []).append(experiment["server"]["command"])

    targets = []
    for value in values:
        if isinstance(value, dict):
            target = TargetHiCacheConfig.from_raw(value)
        elif value in servers:
            policies = [extract_hicache_from_server_command(command) for command in servers[value]]
            if policies[0] is None or any(policy != policies[0] for policy in policies[1:]):
                raise ValueError(f"target label needs one usable HiCache configuration: {value}")
            target = TargetHiCacheConfig.from_raw({"name": value, "hicache": policies[0]})
        else:
            target = TargetHiCacheConfig.load(Path(value))
        targets.append(target)

    if not targets or len({target.label for target in targets}) != len(targets):
        raise ValueError("provide nonempty target_configs with unique names")
    return tuple(targets)


def predict_group(
    scenario: ScenarioRequest,
    targets: tuple[TargetHiCacheConfig, ...],
    summary: dict[str, Any],
    *,
    database_refreshed: bool,
    model_run_jobs: int,
    trace_threads: int,
    trace_file_threads: int,
    diagnostics: str,
    experiment_reports: bool,
) -> None:
    """Optional cost recovery is application orchestration, outside prediction."""

    acquisition = None
    while True:
        summary.pop("undeclared_operator_models", None)
        summary.pop("cost_corrections_required", None)
        summary.update(status="predicting", prediction=None, prediction_summary=None, predictions_completed=0)
        write_json(scenario.output_dir / "group_summary.json", summary)
        inputs = PredictionInputs(
            tuple(source.manifest_path for source in scenario.sources),
            scenario.cost_model,
            tuple(require_repo_path(path) for path in summary["cpu_service_files"]),
            source_environment(scenario.sources[0], scenario.resource_environment)["resources"],
        )
        outcome = invoke_prediction(
            inputs,
            targets,
            scenario.output_dir / "predictions",
            jobs=model_run_jobs,
            trace_threads=trace_threads,
            trace_file_threads=trace_file_threads,
            diagnostics=diagnostics,
            experiment_reports=experiment_reports,
        )
        summary.update(
            prediction_return_code=outcome.return_code,
            prediction_wall_seconds=outcome.wall_seconds,
            cost_model=str(repo_relative_path(scenario.cost_model)),
            status="prediction_failed",
        )
        if outcome.summary is None:
            return

        prediction = outcome.summary["prediction"]
        # Detailed query provenance has one home in workflow_summary.json.
        compact = dict(prediction)
        compact["cells"] = [
            {
                key: row[key]
                for key in (
                    "source_config_id",
                    "target_config",
                    "workload_id",
                    "status",
                    "http_e2e_us",
                    "cost_coverage",
                    "approximations",
                    "blockers",
                    "next_action",
                    "failure_log",
                )
            }
            for row in prediction["cells"]
        ]
        summary.update(
            prediction=compact,
            prediction_summary=str(repo_relative_path(outcome.report_path)),
            predictions_completed=prediction["completed_count"],
            status="predicted"
            if outcome.return_code == 0 and prediction["status"] == "EXECUTED"
            else "prediction_incomplete",
        )
        if prediction["structure_gaps"]:
            summary["status"] = "structure_corrections_required"
            return

        needs = prediction["missing_costs"]
        if not needs or not scenario.automatic_cost_acquisition:
            return

        if acquisition is None:
            from markov_internal.preparation.acquisition import CostAcquisition

            acquisition = CostAcquisition(scenario, database_refreshed=database_refreshed)

        summary["status"] = "preparing_costs"
        write_json(scenario.output_dir / "group_summary.json", summary)
        acquired = acquisition.advance(needs)
        summary["captures"].extend(acquired.pop("captures", []))
        summary.update(acquired)
        if acquired["status"] not in {"database_refreshed", "costs_prepared"}:
            return
        scenario = acquisition.group


def prepare(
    scenario: ScenarioRequest,
    targets: tuple[TargetHiCacheConfig, ...],
    *,
    dry_run: bool = False,
    calibration_only: bool = False,
    model_run_jobs: int = 1,
    trace_threads: int = 1,
    trace_file_threads: int = 1,
    diagnostics: str = "off",
    experiment_reports: bool = False,
) -> dict[str, Any]:
    """Keep the public prepare-hicache command as a thin composition of stages."""

    started = time.monotonic()
    summary: dict[str, Any] = {
        "base_config": scenario.base_config,
        "status": "preparing_inputs",
        "input_summary": str(repo_relative_path(scenario.output_dir / "input_summary.json")),
        "prediction_count": len(scenario.workload_ids) * len(targets),
        "predictions_completed": 0,
        "automatic_cost_acquisition": scenario.automatic_cost_acquisition,
        "captures": [],
    }
    path = scenario.output_dir / "group_summary.json"
    if not dry_run and not calibration_only:
        (scenario.output_dir / "predictions" / "workflow_summary.json").unlink(missing_ok=True)
    write_json(path, summary)
    try:
        database_refreshed = scenario.cost_model is None
        prepared = prepare_inputs(scenario, dry_run=dry_run, prepare_costs=not calibration_only)
        summary.update(prepared.summary)
        if dry_run or calibration_only or prepared.prediction_inputs is None:
            return summary

        predict_group(
            prepared.scenario,
            targets,
            summary,
            database_refreshed=database_refreshed,
            model_run_jobs=model_run_jobs,
            trace_threads=trace_threads,
            trace_file_threads=trace_file_threads,
            diagnostics=diagnostics,
            experiment_reports=experiment_reports,
        )
        return summary
    except KeyboardInterrupt:
        summary.update(status="interrupted", stop_reason="本次已中断，已完成采集保留，可显式重跑继续准备。")
        raise
    except BaseException as error:
        summary.update(status="failed", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        summary["prepare_wall_seconds"] = time.monotonic() - started
        write_json(path, summary)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--group", required=True, type=Path)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--calibration-only", action="store_true", help="prepare source/CPU inputs; no targets required"
    )
    parser.add_argument("--model-run-jobs", type=positive_int, default=1)
    parser.add_argument("--trace-threads", type=positive_int, default=1)
    parser.add_argument("--trace-file-threads", type=positive_int, default=1)
    parser.add_argument("--experiment-reports", action="store_true")
    parser.add_argument("--diagnostics", choices=tuple(level.value for level in DiagnosticLevel), default="off")
    args = parser.parse_args(argv)
    if running_in_modeling_container():
        raise SystemExit("input preparation coordinates containers and must run on the host")

    scenario = ScenarioRequest.load(require_repo_path(args.group))
    if reason := pending_group_capture(scenario.output_dir):
        raise SystemExit(reason)
    targets = () if args.calibration_only else load_targets(scenario)
    try:
        result = prepare(
            scenario,
            targets,
            dry_run=args.dry_run,
            calibration_only=args.calibration_only,
            model_run_jobs=args.model_run_jobs,
            trace_threads=args.trace_threads,
            trace_file_threads=args.trace_file_threads,
            diagnostics=args.diagnostics,
            experiment_reports=args.experiment_reports,
        )
    except KeyboardInterrupt:
        print(f"group={scenario.base_config} status=interrupted; completed captures retained")
        return 130

    print(
        f"group={result['base_config']} status={result['status']} predictions={result['predictions_completed']}/{result['prediction_count']}"
    )
    if result.get("stop_reason"):
        print(result["stop_reason"])
    print(f"report={repo_relative_path(scenario.output_dir / 'group_summary.json')}")
    return 0 if result["status"] in {"planned", "source_inputs_prepared", "predicted"} else 2


if __name__ == "__main__":
    raise SystemExit(main())
