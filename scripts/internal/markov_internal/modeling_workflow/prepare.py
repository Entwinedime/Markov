"""Host orchestration for base, one fixed calibration, model build and prediction."""

from __future__ import annotations

import argparse
from dataclasses import asdict
import os
from pathlib import Path
import subprocess
import time

from ..common.io import load_json, write_json
from ..common.paths import ROOT_DIR, repo_relative_path, require_repo_path, running_in_modeling_container
from .artifacts import DiagnosticLevel, WorkflowArtifactLayout
from .base_capture import base_attempts, capture_base
from .capture import capture_calibration, capture_usage, has_pending_capture, profile_usage
from .fixed_calibration import plan_fixed_calibration
from .group import GroupRequest
from .options import positive_int
from .physical_capture import capture_physical, physical_attempts, physical_usage


def _relative(path: Path) -> str:
    return str(repo_relative_path(path))


def _scan_observations(group: GroupRequest, *, refresh: bool = False) -> dict:
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
    subprocess.run(command, cwd=ROOT_DIR, check=True)
    return load_json(group.output_dir / "calibration_plan.json")


def build_and_predict(group: GroupRequest, summary: dict, *, model_run_jobs: int, diagnostics: str) -> None:
    """Build once, then use target configs only for C++ prediction."""

    entry = str(ROOT_DIR / "scripts/model.sh")
    summary.update(status="building_model", accuracy_verified=False)
    write_json(group.output_dir / "group_summary.json", summary)
    started = time.monotonic()
    result = subprocess.run([entry, "build-hicache-model", "--group", _relative(group.path)], cwd=ROOT_DIR)
    summary["model_build_wall_seconds"] = time.monotonic() - started
    if result.returncode != 0:
        summary.update(status="model_build_failed", model_build_return_code=result.returncode)
        return

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
    for index, target in enumerate(group.targets):
        path = group.output_dir / "targets" / f"target_{index + 1}.json"
        write_json(path, {"name": target.label, "hicache": target.fields})
        command.extend(("--target-config", _relative(path)))
    summary["status"] = "predicting"
    write_json(group.output_dir / "group_summary.json", summary)
    started = time.monotonic()
    result = subprocess.run(command, cwd=ROOT_DIR)
    summary.update(prediction_wall_seconds=time.monotonic() - started, prediction_return_code=result.returncode)
    if result.returncode != 0:
        summary["status"] = "prediction_failed"
        return
    path = WorkflowArtifactLayout(output).workflow_summary_path
    prediction = load_json(path)["prediction"]
    summary.update(prediction=prediction, prediction_summary=_relative(path), predictions_completed=prediction["ready_count"])
    complete = prediction["status"] == "READY" and prediction["ready_count"] == summary["prediction_count"]
    summary["status"] = "predicted" if complete else "prediction_incomplete"


def prepare(group: GroupRequest, *, dry_run: bool, refresh_observations: bool = False,
            model_run_jobs: int = 1, diagnostics: str = DiagnosticLevel.OFF.value,
            calibration_only: bool = False) -> dict:
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
            attempts=attempts,
        )
        attempt["status"] = summary["status"]
        return summary
    except BaseException as error:
        attempt.update(status="interrupted" if isinstance(error, (KeyboardInterrupt, SystemExit)) else "prepare_failed",
                       error=f"{type(error).__name__}: {error}")
        summary.update(status=attempt["status"], error=attempt["error"])
        raise
    finally:
        attempt["wall_seconds"] = time.monotonic() - started
        summary["preparation_attempts"] = attempts
        write_json(summary_path, summary)


def _prepare_once(group: GroupRequest, *, dry_run: bool, refresh_observations: bool,
                  model_run_jobs: int, diagnostics: str, calibration_only: bool,
                  attempts: list[dict]) -> dict:
    started = time.monotonic()
    group.output_dir.mkdir(parents=True, exist_ok=True)
    summary_path = group.output_dir / "group_summary.json"
    base_plan = capture_base(group, dry_run=dry_run) if group.missing_base_workloads else None
    if base_plan and base_plan["status"] == "base_captured":
        group = GroupRequest.load(group.path)
    physical_plan = (
        capture_physical(group, dry_run=dry_run)
        if not group.missing_base_workloads and group.physical is None
        else None
    )
    if physical_plan and physical_plan["status"] == "physical_captured":
        group = GroupRequest.load(group.path)

    summary = {
        "base_config": group.base_config,
        "status": "preparing",
        "preparation_attempts": attempts,
        "workloads": list(group.workload_ids),
        "target_configs": [target.label for target in group.targets],
        "prediction_count": len(group.workload_ids) * len(group.targets),
        "predictions_completed": 0,
        "base_manifests": [_relative(source.manifest_path) for source in group.sources],
        "budget": asdict(group.budget),
        "dry_run": dry_run,
        "calibration_only": calibration_only,
        "target_score_inputs": [],
        "base_capture_usage": profile_usage(base_attempts(group.raw)),
        "physical_capture_usage": physical_usage(physical_attempts(group)),
    }
    if base_plan and base_plan["status"] != "base_captured":
        summary.update(status=base_plan["status"], base_plan=base_plan)
        return summary
    if physical_plan and physical_plan["status"] != "physical_captured":
        summary.update(status=physical_plan["status"], physical_plan=physical_plan)
        return summary
    if group.missing_base_workloads or group.physical is None:
        summary["status"] = "needs_source_data"
        return summary

    calibration = plan_fixed_calibration(group)
    summary["fixed_calibration"] = calibration
    observation_plan = _scan_observations(group, refresh=refresh_observations)
    if not dry_run and calibration["status"] == "needs_calibration":
        summary["captures"] = []
        while calibration["remaining_profile_count"]:
            captured = capture_calibration(group, calibration["definition"], calibration["next_point"])
            summary["captures"].append(captured)
            if captured["status"] != "captured":
                break
            observation_plan = _scan_observations(group)
            calibration = plan_fixed_calibration(group)
            summary["fixed_calibration"] = calibration

    ledger_path = group.output_dir / "capture_ledger.json"
    if ledger_path.exists():
        summary["calibration_usage"] = capture_usage(load_json(ledger_path)["attempts"])
    summary["model_inputs"] = observation_plan["model_inputs"]
    if calibration["status"] == "input_conflict":
        summary["status"] = "calibration_input_conflict"
    elif observation_plan["model_inputs"]["status"] != "ready":
        summary["status"] = "needs_calibration_data"
    else:
        summary["status"] = "ready_for_model_build"
    summary["prepare_wall_seconds"] = time.monotonic() - started
    write_json(summary_path, summary)
    if not dry_run and not calibration_only and summary["status"] == "ready_for_model_build":
        build_and_predict(group, summary, model_run_jobs=model_run_jobs, diagnostics=diagnostics)
    summary["prepare_wall_seconds"] = time.monotonic() - started
    write_json(summary_path, summary)
    return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Prepare one SGLang base with one repeatable fixed calibration.")
    parser.add_argument("--group", required=True, type=Path)
    parser.add_argument("--dry-run", action="store_true", help="write the fixed calibration and model-input plan without inference")
    parser.add_argument("--calibration-only", action="store_true", help="prepare data without building or predicting")
    parser.add_argument("--refresh-observations", action="store_true")
    parser.add_argument("--model-run-jobs", type=positive_int, default=1)
    parser.add_argument("--diagnostics", choices=tuple(level.value for level in DiagnosticLevel), default=DiagnosticLevel.OFF.value)
    args = parser.parse_args(argv)
    if running_in_modeling_container():
        raise SystemExit("prepare-hicache coordinates containers and must run on the host")
    group = GroupRequest.load(require_repo_path(args.group))
    if has_pending_capture(group.output_dir):
        raise SystemExit("a recorded capture is still running or needs cleanup")
    result = prepare(
        group,
        dry_run=args.dry_run,
        refresh_observations=args.refresh_observations,
        model_run_jobs=args.model_run_jobs,
        diagnostics=args.diagnostics,
        calibration_only=args.calibration_only,
    )
    print(f"group={result['base_config']} status={result['status']} predictions={result['predictions_completed']}/{result['prediction_count']}")
    complete = result["status"] == ("ready_for_model_build" if args.calibration_only else "predicted")
    return 0 if args.dry_run or complete else 2


if __name__ == "__main__":
    raise SystemExit(main())
