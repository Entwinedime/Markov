"""Host orchestration for base evidence, shared missing-cost acquisition and prediction."""

from __future__ import annotations

import argparse
from collections.abc import Sequence
from dataclasses import asdict
import os
from pathlib import Path
import subprocess
import time

from ..common.io import load_json, write_json
from ..common.paths import ROOT_DIR, repo_relative_path, require_repo_path, running_in_modeling_container
from .context import DiagnosticLevel
from .base_capture import base_attempts, capture_base
from .capture import capture_calibration, capture_usage, pending_group_capture, profile_usage
from .fixed_calibration import materialize_fixed_calibration, plan_fixed_calibration
from .group import GroupRequest
from .group_cpu_service import cpu_service_plan, prepare_group_cpu_service
from .control_acquisition import prepare_control_calibration
from ..common.commands import positive_int
from .physical_capture import SERVICE_SAMPLERS, capture_physical, physical_attempts, physical_usage, prepare_platform


def _relative(path: Path) -> str:
    return str(repo_relative_path(path))


def _scan_model_inputs(
    group: GroupRequest, summary: dict, *, build_model: bool, refresh: bool = False, cpu_services: Sequence[Path] = ()
) -> dict:
    summary["status"] = "preparing_model" if build_model else "checking_model_inputs"
    write_json(group.output_dir / "group_summary.json", summary)
    command = [
        str(ROOT_DIR / "scripts/run.sh"),
        "modeling",
        "--",
        "env",
        "PYTHONPATH=scripts/internal",
        "python3",
        "-m",
        "markov_internal.modeling_workflow.observations",
        "--group",
        _relative(group.path),
    ]
    if refresh:
        command.append("--refresh")
    if build_model:
        command.append("--build-model")
    for service in cpu_services:
        command.extend(("--cpu-service-cost", _relative(service)))
    started = time.monotonic()
    subprocess.run(command, cwd=ROOT_DIR, check=True)
    summary["model_preparation_wall_seconds"] = (
        summary.get("model_preparation_wall_seconds", 0.0) + time.monotonic() - started
    )
    inputs = load_json(group.output_dir / "model_inputs.json")
    summary["model_inputs"] = {"status": inputs["status"], "missing": inputs.get("missing", [])}
    if inputs.get("service_gaps"):
        summary["model_inputs"]["service_gaps"] = inputs["service_gaps"]
    return inputs


def predict_group(group: GroupRequest, summary: dict, *, model_run_jobs: int, diagnostics: str) -> None:
    """Resolve supported operation gaps once per group, never from target scores."""
    # Acquisition imports group definitions; keep this orchestration dependency local.
    from .calibration.eviction_cpu import acquire_eviction_cpu

    attempted = set()
    while True:
        _predict(group, summary, model_run_jobs=model_run_jobs, diagnostics=diagnostics)
        needs = summary.get("prediction", {}).get("missing_costs", [])
        components = {need["component"] for need in needs}
        # Execution names the missing service. The sampler owns the supported
        # acquisition domain; the build-time coverage report is diagnostic only.
        service_needs = (components & SERVICE_SAMPLERS.keys()) - attempted
        # A declared release was already offered to execution. Its unresolved
        # gap must not prevent other targets from acquiring independent costs.
        if group.raw.get("control_calibrations", {}).get("release_host"):
            attempted.add("release_regular")
        operations = ["io_service"] if service_needs else []
        operations.extend(
            name
            for name in ("eviction_locked_candidate", "release_regular")
            if "execution_control/" + name in components and name not in attempted
        )
        operations.extend(
            sorted(
                {
                    coordinate["program"]
                    for need in needs
                    if need["component"] == "execution_control/prefetch"
                    for coordinate in need["coordinates"]
                }
                - attempted
            )
        )
        changed = stopped = False
        prediction_status = summary["status"]
        for operation in operations:
            attempted.update(service_needs if operation == "io_service" else [operation])
            summary["status"] = "acquiring_execution_cost"
            write_json(group.output_dir / "group_summary.json", summary)
            if operation == "io_service":
                acquisition = capture_physical(group, dry_run=False, required_components=service_needs)
                summary["physical_capture_usage"] = physical_usage(physical_attempts(group))
                acquired = acquisition["status"] == "physical_captured" or bool(acquisition.get("completed_components"))
            elif operation in {"release_regular", "best_effort", "wait_complete", "local_return"}:
                release = operation == "release_regular"
                policy = "best_effort" if operation == "local_return" else operation
                definition = materialize_fixed_calibration(
                    group, **({"release_only": True} if release else {"prefetch_policy": policy})
                )
                acquisition = prepare_control_calibration(
                    group, dict(definition=definition, operation="release_host" if release else "prefetch_wait")
                )
                acquired = acquisition["status"] == "control_calibrated"
            else:
                acquisition = acquire_eviction_cpu(group, needs)
                acquired = acquisition["status"] == "cpu_primitive_measured" and not acquisition["reused"]
            summary.setdefault("captures", []).append(dict(acquisition, operation=operation))
            changed |= acquired
            complete = acquisition["status"] in {"physical_captured", "control_calibrated", "cpu_primitive_measured"}
            if not complete and acquisition["status"] not in {
                "physical_budget_exhausted",
                "budget_exhausted",
                "needs_physical_calibration",
                "needs_cpu_primitive",
                "physical_budget_required",
            }:
                # Each demand checks the cumulative ledger itself: a smaller
                # experiment can fit after a larger one was denied. Only real
                # process/artifact/cleanup failures stop later acquisitions.
                stopped = True
                break
            if acquired:
                group = GroupRequest.load(group.path)
        summary["status"] = prediction_status
        if not changed:
            return

        # A shared domain is tried once. If it still leaves the same gap, retain
        # that failure rather than remeasuring or fitting a target-specific fix.
        for key in ("prediction", "prediction_summary", "prediction_return_code"):
            summary.pop(key, None)
        summary["predictions_completed"] = 0
        summary["status"] = "building_model"
        group = GroupRequest.load(group.path)
        inputs = _scan_model_inputs(
            group,
            summary,
            build_model=True,
            cpu_services=[require_repo_path(path) for path in summary.get("cpu_service_files", [])],
        )
        if inputs["status"] != "ready":
            summary["status"] = "needs_calibration_data"
            return
        if stopped:
            _predict(group, summary, model_run_jobs=model_run_jobs, diagnostics=diagnostics)
            return


def _predict(group: GroupRequest, summary: dict, *, model_run_jobs: int, diagnostics: str) -> None:
    """Predict the whole group from the model prepared in this invocation."""
    for key in ("prediction", "prediction_summary", "prediction_return_code"):
        summary.pop(key, None)
    summary["predictions_completed"] = 0

    entry = str(ROOT_DIR / "scripts/model.sh")
    service_inputs = summary.get("cpu_service_files", [])

    output = group.output_dir / "predictions"
    command = [
        entry,
        "predict-hicache",
        "--hicache-io-model",
        _relative(group.output_dir / "hicache_io_model.json"),
        "--output-dir",
        _relative(output),
        "--model-run-jobs",
        str(model_run_jobs),
        "--diagnostics",
        diagnostics,
        "--continue-on-error",
    ]
    for source in group.sources:
        command.extend(("--source-manifest", _relative(source.manifest_path)))
    for service in service_inputs:
        command.extend(("--cpu-service-cost", _relative(service)))
    for index, target in enumerate(group.targets):
        path = group.output_dir / "targets" / f"target_{index + 1}.json"
        write_json(path, {"name": target.label, "hicache": target.fields})
        command.extend(("--target-config", _relative(path)))
    summary["status"] = "predicting"
    write_json(group.output_dir / "group_summary.json", summary)
    started = time.monotonic()
    report = output / "workflow_summary.json"
    # This is the current-attempt report, not a cache. Removing it avoids
    # mistaking an old report for a result when the new command fails early.
    report.unlink(missing_ok=True)
    result = subprocess.run(command, cwd=ROOT_DIR)
    summary.update(
        prediction_wall_seconds=summary.get("prediction_wall_seconds", 0.0) + time.monotonic() - started,
        prediction_return_code=result.returncode,
    )
    summarize_prediction(summary, output)


def summarize_prediction(summary: dict, output: Path) -> None:
    """Keep partial results, but never attribute an old report to a failed attempt."""
    path = output / "workflow_summary.json"
    if not path.is_file():
        summary["status"] = "prediction_failed"
        return
    prediction = load_json(path)["prediction"]
    completed = prediction["completed_count"]
    summary.update(
        prediction={key: value for key, value in prediction.items() if key != "cells"},
        prediction_summary=_relative(path),
        predictions_completed=completed,
    )
    complete = (
        summary["prediction_return_code"] == 0
        and prediction["status"] == "EXECUTED"
        and completed == summary["prediction_count"]
    )
    summary["status"] = "predicted" if complete else "prediction_incomplete"


def prepare(
    group: GroupRequest,
    *,
    dry_run: bool,
    refresh_observations: bool = False,
    model_run_jobs: int = 1,
    diagnostics: str = DiagnosticLevel.OFF.value,
    calibration_only: bool = False,
) -> dict:
    """Run the single forward workflow and preserve inclusive attempt accounting."""

    started = time.monotonic()
    summary_path = group.output_dir / "group_summary.json"
    previous = load_json(summary_path) if summary_path.exists() else {}
    attempts = previous.get("preparation_attempts", [])
    attempt = {
        "pid": os.getpid(),
        "started_at_unix": time.time(),
        "status": "running",
        "wall_seconds": None,
        "dry_run": dry_run,
        "calibration_only": calibration_only,
        "clock_scope": "inclusive_host_prepare_function",
    }
    attempts.append(attempt)
    summary = {"base_config": group.base_config, "status": "preparing", "preparation_attempts": attempts}
    write_json(summary_path, summary)
    try:
        summary = _prepare_once(
            group,
            dry_run=dry_run,
            refresh_observations=refresh_observations,
            model_run_jobs=model_run_jobs,
            diagnostics=diagnostics,
            calibration_only=calibration_only,
            summary=summary,
        )
        attempt["status"] = summary["status"]
        return summary
    except BaseException as error:
        attempt.update(
            failed_stage=summary["status"],
            status="interrupted" if isinstance(error, (KeyboardInterrupt, SystemExit)) else "prepare_failed",
            error=f"{type(error).__name__}: {error}",
        )
        summary.update(status=attempt["status"], error=attempt["error"])
        raise
    finally:
        attempt["wall_seconds"] = time.monotonic() - started
        summary["prepare_wall_seconds"] = attempt["wall_seconds"]
        summary["physical_capture_usage"] = physical_usage(physical_attempts(group))
        ledger_path = group.output_dir / "capture_ledger.json"
        if ledger_path.exists():
            summary["calibration_usage"] = capture_usage(load_json(ledger_path)["attempts"])
        write_json(summary_path, summary)


def _prepare_once(
    group: GroupRequest,
    *,
    dry_run: bool,
    refresh_observations: bool,
    model_run_jobs: int,
    diagnostics: str,
    calibration_only: bool,
    summary: dict,
) -> dict:
    summary_path = group.output_dir / "group_summary.json"
    build_model = not dry_run and not calibration_only
    base_plan = capture_base(group, dry_run=dry_run) if group.missing_base_workloads else None
    if base_plan and base_plan["status"] == "base_captured":
        group = GroupRequest.load(group.path)
    summary.update(
        {
            "base_config": group.base_config,
            "status": "preparing",
            "workloads": list(group.workload_ids),
            "target_configs": [target.label for target in group.targets],
            "prediction_count": len(group.workload_ids) * len(group.targets),
            "predictions_completed": 0,
            "base_manifests": [_relative(source.manifest_path) for source in group.sources],
            "cpu_service": cpu_service_plan(group),
            "budget": asdict(group.budget),
            "dry_run": dry_run,
            "calibration_only": calibration_only,
            "base_capture_usage": profile_usage(base_attempts(group.raw)),
            "physical_capture_usage": physical_usage(physical_attempts(group)),
        }
    )
    if base_plan and base_plan["status"] != "base_captured":
        summary.update(status=base_plan["status"], base_plan=base_plan)
        return summary
    if group.missing_base_workloads:
        summary["status"] = "needs_source_data"
        return summary
    if group.physical is None:
        platform = prepare_platform(group)
        if platform["status"] != "platform_ready":
            summary.update(status=platform["status"], physical_plan=platform)
            return summary
        group = GroupRequest.load(group.path)

    if not dry_run:
        summary["status"] = "preparing_cpu_service"
        write_json(summary_path, summary)
    services = prepare_group_cpu_service(summary["cpu_service"], dry_run=dry_run)
    summary["cpu_service_files"] = [_relative(path) for path in services]
    cpu_status = summary["cpu_service"]["status"]
    if cpu_status not in {"prepared", "not_requested"}:
        summary["status"] = {
            "needs_preparation": "needs_cpu_preparation",
            "needs_capture_inputs": "needs_cpu_capture_inputs",
        }.get(cpu_status, cpu_status)
        return summary
    inputs = _scan_model_inputs(
        group, summary, build_model=build_model, refresh=refresh_observations, cpu_services=services
    )

    while True:
        previous_missing = inputs.get("missing", [])
        calibration = plan_fixed_calibration(group, inputs)
        summary["fixed_calibration"] = calibration
        if inputs["status"] == "ready":
            summary["status"] = "ready_for_prediction" if build_model else "ready_for_model_build"
            break
        if dry_run or calibration["status"] != "needs_calibration":
            summary["status"] = calibration["status"]
            break

        summary["status"] = "capturing_calibration"
        write_json(summary_path, summary)
        acquired = capture_calibration(group, calibration["definition"])
        summary.setdefault("captures", []).append(dict(acquired, operation="phase"))
        if acquired["status"] != "captured":
            summary["status"] = acquired["status"]
            break

        group = GroupRequest.load(group.path)
        inputs = _scan_model_inputs(group, summary, build_model=build_model, cpu_services=services)
        if inputs["status"] != "ready" and inputs.get("missing", []) == previous_missing:
            summary["status"] = "experiment_exhausted"
            summary["fixed_calibration"] = {
                "status": "experiment_exhausted",
                "missing": inputs.get("missing", []),
                "reason": "The shared experiment did not resolve any cost requirement; repeating it is not justified.",
            }
            break

    write_json(summary_path, summary)
    if summary["status"] == "ready_for_prediction":
        predict_group(group, summary, model_run_jobs=model_run_jobs, diagnostics=diagnostics)
    return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Prepare a SGLang base, acquire missing shared evidence and predict its targets."
    )
    parser.add_argument("--group", required=True, type=Path)
    parser.add_argument(
        "--dry-run", action="store_true", help="check model inputs and plan any needed acquisition without inference"
    )
    parser.add_argument("--calibration-only", action="store_true", help="prepare data without building or predicting")
    parser.add_argument("--refresh-observations", action="store_true")
    parser.add_argument("--model-run-jobs", type=positive_int, default=1)
    parser.add_argument(
        "--diagnostics", choices=tuple(level.value for level in DiagnosticLevel), default=DiagnosticLevel.OFF.value
    )
    args = parser.parse_args(argv)
    if running_in_modeling_container():
        raise SystemExit("prepare-hicache coordinates containers and must run on the host")
    group = GroupRequest.load(require_repo_path(args.group))
    if reason := pending_group_capture(group.output_dir):
        raise SystemExit(reason)
    result = prepare(
        group,
        dry_run=args.dry_run,
        refresh_observations=args.refresh_observations,
        model_run_jobs=args.model_run_jobs,
        diagnostics=args.diagnostics,
        calibration_only=args.calibration_only,
    )
    print(
        f"group={result['base_config']} status={result['status']} predictions={result['predictions_completed']}/{result['prediction_count']}"
    )
    cpu = result["cpu_service"]
    if required := cpu.get("capture_required"):
        print(
            f"Base CPU replay size: {required['server_starts']} server start, "
            f"{required['requests']} requests and {required['tokens']} tokens (one source, not a whole-group total)"
        )

    for gap in result.get("model_inputs", {}).get("missing", []):
        print(f"unresolved cost: {gap['component']}: {gap['reason']}")
    for gap in result.get("model_inputs", {}).get("service_gaps", []):
        print(f"I/O cost unavailable; acquired only if executed: {gap['component']}: {gap['reason']}")
    for gap in result.get("prediction", {}).get("missing_costs", []):
        print(f"unresolved execution cost: {gap['component']}: {'; '.join(gap['reasons'])}")

    stages = [
        (label, result[key])
        for label, key in (
            ("base capture", "base_plan"),
            ("base CPU preparation", "cpu_service"),
            ("physical calibration", "physical_plan"),
            ("shared experiment", "fixed_calibration"),
        )
        if key in result
    ]
    stages.extend((f"shared acquisition ({row['operation']})", row) for row in result.get("captures", []))
    for label, stage in stages:
        print(f"{label}: {stage['status']}")
        for details in (stage, stage.get("attempt", {})):
            for field in (
                "stop_reason",
                "reason",
                "error",
                "artifact_error",
                "cleanup_error",
                "returncode",
                "command_log",
            ):
                if field in details and details[field] is not None:
                    print(f"  {field}: {details[field]}")
            for field in ("limits", "requirements", "limitations"):
                for detail in details.get(field, []):
                    print(f"  {field}: {detail}")
        for limitation in stage.get("bootstrap", {}).get("limitations", []):
            print(f"  limitations: {limitation}")

    print(f"report={_relative(group.output_dir / 'group_summary.json')}")
    complete = result["status"] == ("ready_for_model_build" if args.calibration_only else "predicted")
    return 0 if args.dry_run or complete else 2


if __name__ == "__main__":
    raise SystemExit(main())
