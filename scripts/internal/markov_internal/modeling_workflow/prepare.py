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
from .base_capture import capture_base
from .capture import pending_group_capture
from .context import DiagnosticLevel
from .group import GroupRequest
from .group_cpu_service import cpu_service_plan, prepare_group_cpu_service


def _relative(path: Path) -> str:
    return str(repo_relative_path(path))


def _predict(
    group: GroupRequest, summary: dict[str, Any], *, jobs: int, diagnostics: str, experiment_reports: bool
) -> None:
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
    for service in summary["cpu_service_files"]:
        command.extend(("--cpu-service-cost", service))
    for index, target in enumerate(group.targets):
        path = group.output_dir / "targets" / f"target_{index + 1}.json"
        write_json(path, {"name": target.label, "hicache": target.fields})
        command.extend(("--target-config", _relative(path)))

    report = output / "workflow_summary.json"
    report.unlink(missing_ok=True)
    # Each attempt replaces the previous result. A launch failure must not
    # reuse stale missing costs to trigger another round of acquisition.
    summary.pop("prediction", None)
    summary.pop("prediction_summary", None)
    summary["predictions_completed"] = 0
    summary["status"] = "predicting"
    started = time.monotonic()
    code = run_command([str(ROOT_DIR / "scripts/model.sh"), "predict", *command]).returncode
    summary["prediction_return_code"] = code
    summary["prediction_wall_seconds"] = time.monotonic() - started
    if not report.is_file():
        summary["status"] = "prediction_failed"
        return

    result = load_json(report)["prediction"]
    summary["prediction"] = {key: value for key, value in result.items() if key != "cells"}
    summary["prediction_summary"] = _relative(report)
    summary["predictions_completed"] = result["completed_count"]
    summary["status"] = "predicted" if code == 0 and result["status"] == "EXECUTED" else "prediction_incomplete"


def predict_group(
    group: GroupRequest,
    summary: dict[str, Any],
    *,
    model_run_jobs: int,
    diagnostics: str,
    experiment_reports: bool = False,
) -> None:
    """Predict directly; enter optional shared recovery only after a cost gap."""

    acquisition = None
    while True:
        summary.pop("undeclared_operator_models", None)
        summary.pop("cost_corrections_required", None)
        _predict(group, summary, jobs=model_run_jobs, diagnostics=diagnostics, experiment_reports=experiment_reports)
        if summary.get("prediction", {}).get("structure_gaps"):
            summary["status"] = "structure_corrections_required"
            return

        needs = summary.get("prediction", {}).get("missing_costs", [])
        if not needs or not group.raw.get("automatic_cost_acquisition", False):
            return

        if acquisition is None:
            # Default prediction does not import capture planners or trainers.
            from .acquisition import CostAcquisition

            acquisition = CostAcquisition(group)

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
            result = capture_base(group, dry_run=dry_run)
            summary["base_plan"] = result
            if result["status"] != "base_captured":
                summary["status"] = result["status"]
                return summary
            group = GroupRequest.load(group.path)

        if not group.raw.get("cost_model") and not calibration_only:
            summary["status"] = "needs_independent_cost_model"
            return summary
        summary["base_manifests"] = [_relative(source.manifest_path) for source in group.sources]
        summary["cpu_service"] = cpu_service_plan(group)
        services = prepare_group_cpu_service(summary["cpu_service"], dry_run=dry_run)
        summary["cpu_service_files"] = [_relative(value) for value in services]
        if summary["cpu_service"]["status"] not in {"prepared", "not_requested"}:
            summary["status"] = summary["cpu_service"]["status"]
            return summary

        if dry_run or calibration_only:
            summary["status"] = "planned" if dry_run else "source_inputs_prepared"
            return summary

        predict_group(
            group,
            summary,
            model_run_jobs=model_run_jobs,
            diagnostics=diagnostics,
            experiment_reports=experiment_reports,
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
    print(f"report={_relative(group.output_dir / 'group_summary.json')}")
    return 0 if result["status"] in {"planned", "source_inputs_prepared", "predicted"} else 2


if __name__ == "__main__":
    raise SystemExit(main())
