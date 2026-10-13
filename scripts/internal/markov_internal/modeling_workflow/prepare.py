"""Host orchestration: acquire a base, consume independent costs, predict targets."""

from __future__ import annotations

import argparse
from pathlib import Path
import time
from typing import Any

from ..common.commands import positive_int
from ..common.io import load_json, write_json
from ..common.paths import ROOT_DIR, repo_relative_path, require_repo_path, running_in_modeling_container
from ..common.process import run_command
from ..modeling.deployment import source_environment
from .capture import pending_group_capture
from .context import DiagnosticLevel
from .group import GroupRequest
from .cpu_service import cpu_service_plan, prepare_cpu_services


def _relative(path: Path) -> str:
    return str(repo_relative_path(path))


def _predict(
    group: GroupRequest, cpu_service_files: list[str], *, jobs: int, diagnostics: str, experiment_reports: bool
) -> dict[str, Any]:
    """Run one attempt and return its outcome; never mutate preparation state."""

    output = group.output_dir / "predictions"
    command = [
        "--cost-model",
        group.raw["cost_model"],
        "--output-dir",
        _relative(output),
        "--model-run-jobs",
        str(jobs),
        "--diagnostics",
        diagnostics,
        "--continue-on-error",
    ]
    if experiment_reports:
        command.append("--experiment-reports")
    resources = source_environment(group.sources[0], group.raw.get("resource_environment"))["resources"]
    if resources is not None:
        resource_path = group.output_dir / "resource_environment.json"
        write_json(resource_path, resources)
        command.extend(("--resource-environment", _relative(resource_path)))
    for source in group.sources:
        command.extend(("--source-manifest", _relative(source.manifest_path)))
    for service in cpu_service_files:
        command.extend(("--cpu-service-cost", service))
    for index, target in enumerate(group.targets):
        path = group.output_dir / "targets" / f"target_{index + 1}.json"
        write_json(path, {"name": target.label, "hicache": target.fields})
        command.extend(("--target-config", _relative(path)))

    report = output / "workflow_summary.json"
    report.unlink(missing_ok=True)
    # Each attempt replaces the previous result. A launch failure must not
    # reuse stale missing costs to trigger another round of acquisition.
    started = time.monotonic()
    code = run_command([str(ROOT_DIR / "scripts/model.sh"), "predict", *command]).returncode
    outcome: dict[str, Any] = {
        "status": "prediction_failed",
        "prediction_return_code": code,
        "prediction_wall_seconds": time.monotonic() - started,
        "cost_model": group.raw["cost_model"],
        "prediction": None,
        "prediction_summary": None,
        "predictions_completed": 0,
    }
    if not report.is_file():
        return outcome

    prediction = load_json(report)["prediction"]
    # One readable group report contains the results and limitations. Detailed
    # query provenance stays in the workflow report, not another full copy.
    prediction["cells"] = [
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
            )
        }
        for row in prediction["cells"]
    ]
    outcome.update(
        prediction=prediction,
        prediction_summary=_relative(report),
        predictions_completed=prediction["completed_count"],
        status="predicted" if code == 0 and prediction["status"] == "EXECUTED" else "prediction_incomplete",
    )
    return outcome


def predict_group(
    group: GroupRequest,
    summary: dict[str, Any],
    *,
    model_run_jobs: int,
    diagnostics: str,
    experiment_reports: bool = False,
    database_refreshed: bool = False,
) -> None:
    """Predict directly; enter optional shared recovery only after a cost gap."""

    acquisition = None
    while True:
        summary.pop("undeclared_operator_models", None)
        summary.pop("cost_corrections_required", None)
        attempt = _predict(
            group,
            summary["cpu_service_files"],
            jobs=model_run_jobs,
            diagnostics=diagnostics,
            experiment_reports=experiment_reports,
        )
        summary.update(attempt)
        prediction = attempt["prediction"]
        if prediction is None:
            return
        if prediction["structure_gaps"]:
            summary["status"] = "structure_corrections_required"
            return

        needs = prediction["missing_costs"]
        if not needs or not group.raw.get("automatic_cost_acquisition", False):
            return

        if acquisition is None:
            # Default prediction does not import capture planners or trainers.
            from .acquisition import CostAcquisition

            acquisition = CostAcquisition(group, database_refreshed=database_refreshed)

        outcome = acquisition.advance(needs)
        summary["captures"].extend(outcome.pop("captures", []))
        summary.update(outcome)
        if outcome["status"] not in {"database_refreshed", "costs_prepared"}:
            return

        group = acquisition.group


def prepare(
    group: GroupRequest,
    *,
    dry_run: bool,
    model_run_jobs: int = 1,
    diagnostics: str = DiagnosticLevel.OFF.value,
    calibration_only: bool = False,
    experiment_reports: bool = False,
) -> dict[str, Any]:
    """Prepare source inputs without fitting their costs; persist one compact result."""

    started = time.monotonic()
    summary: dict[str, Any] = {
        "base_config": group.base_config,
        "status": "preparing",
        "prediction_count": len(group.workload_ids) * len(group.targets),
        "predictions_completed": 0,
        "automatic_cost_acquisition": bool(group.raw.get("automatic_cost_acquisition", False)),
        "captures": [],
    }
    path = group.output_dir / "group_summary.json"
    try:
        if group.missing_base_workloads:
            # Existing sources do not need the capture planners.
            from .profile_capture import capture_base

            result = capture_base(group, dry_run=dry_run)
            summary["base_plan"] = result
            if result["status"] != "base_captured":
                summary["status"] = "planned" if dry_run else result["status"]
                return summary
            group = GroupRequest.load(group.path)

        database_inputs = bool(group.raw.get("cost_database") and group.raw.get("cost_training"))
        if not group.raw.get("cost_model") and not database_inputs and not calibration_only:
            summary["status"] = "needs_independent_cost_model"
            summary["stop_reason"] = "Provide cost_model, or cost_database and cost_training for offline preparation."
            return summary
        summary["base_manifests"] = [_relative(source.manifest_path) for source in group.sources]
        summary["cpu_service"] = cpu_service_plan(group.sources, group.raw, group.output_dir)
        services = prepare_cpu_services(summary["cpu_service"], dry_run=dry_run)
        summary["cpu_service_files"] = [_relative(value) for value in services]
        if summary["cpu_service"]["status"] not in {"prepared", "not_requested"}:
            summary["status"] = summary["cpu_service"]["status"]
            return summary

        if dry_run or calibration_only:
            summary["status"] = "planned" if dry_run else "source_inputs_prepared"
            return summary

        database_refreshed = not group.raw.get("cost_model")
        if database_refreshed:
            # Only an explicit database/training declaration selects this path.
            # Existing packages stay the normal path and are never refitted.
            from .cost_preparation import train_group_cost_model

            trained = train_group_cost_model(group, load_json(require_repo_path(group.raw["cost_training"])))
            if trained is None:
                summary["status"] = "shared_training_incomplete"
                return summary

            group = trained

        predict_group(
            group,
            summary,
            model_run_jobs=model_run_jobs,
            diagnostics=diagnostics,
            experiment_reports=experiment_reports,
            database_refreshed=database_refreshed,
        )
        return summary
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
    parser.add_argument("--calibration-only", action="store_true", help="stop after source input preparation")
    parser.add_argument("--model-run-jobs", type=positive_int, default=1)
    parser.add_argument(
        "--experiment-reports", action="store_true", help="retain source replay and executed structure reports"
    )
    parser.add_argument(
        "--diagnostics", choices=tuple(level.value for level in DiagnosticLevel), default=DiagnosticLevel.OFF.value
    )
    args = parser.parse_args(argv)
    if running_in_modeling_container():
        raise SystemExit("group preparation coordinates containers and must run on the host")
    group = GroupRequest.load(require_repo_path(args.group))
    if reason := pending_group_capture(group.output_dir):
        raise SystemExit(reason)
    result = prepare(
        group,
        dry_run=args.dry_run,
        model_run_jobs=args.model_run_jobs,
        diagnostics=args.diagnostics,
        calibration_only=args.calibration_only,
        experiment_reports=args.experiment_reports,
    )
    print(
        f"group={result['base_config']} status={result['status']} "
        f"predictions={result['predictions_completed']}/{result['prediction_count']}"
    )
    if result.get("stop_reason"):
        print(result["stop_reason"])
    print(f"report={_relative(group.output_dir / 'group_summary.json')}")
    return 0 if result["status"] in {"planned", "source_inputs_prepared", "predicted"} else 2


if __name__ == "__main__":
    raise SystemExit(main())
