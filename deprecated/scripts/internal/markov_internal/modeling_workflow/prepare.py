"""Host orchestration: acquire a base, consume independent costs, predict targets."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
import time
from typing import Any

from ..common.commands import positive_int
from ..common.io import load_json, write_json
from ..common.paths import ROOT_DIR, repo_relative_path, require_repo_path, running_in_modeling_container
from ..modeling.cost.package import control_evidence
from ..modeling.deployment import source_environment
from .base_capture import capture_base
from .calibration.eviction_cpu import locked_candidate_observations
from .capture import completed_profile, pending_group_capture
from .context import DiagnosticLevel
from .control_acquisition import (
    capture_paired_calibration,
    declared_cost_demands,
    operator_requirements,
    pending_control_experiments,
    prepare_control_calibration,
)
from .fixed_calibration import materialize_fixed_calibration
from .group import GroupRequest
from .group_cpu_service import cpu_service_plan, prepare_group_cpu_service
from .physical_capture import SERVICE_SAMPLERS, capture_physical


def _relative(path: Path) -> str:
    return str(repo_relative_path(path))


def _run_action(action: str, *arguments: str) -> int:
    return subprocess.run([str(ROOT_DIR / "scripts/model.sh"), action, *arguments], cwd=ROOT_DIR).returncode


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
    code = _run_action("predict", *command)
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


def _import_acquired(group: GroupRequest, acquisitions: list[dict[str, Any]]) -> bool:
    """Admit new independent captures, then explicitly retrain one shared model.

    The offline request excludes every selected base. Acquisition never uses
    scores, target traces or per-cell fitted corrections.
    """

    database = group.raw["cost_database"]
    request = load_json(require_repo_path(group.raw["cost_training"]))
    ledger_path = group.output_dir / "capture_ledger.json"
    deployment = source_environment(group.sources[0], group.raw.get("resource_environment"))
    resource_path = group.output_dir / "resource_environment.json"
    if deployment["resources"] is not None:
        write_json(resource_path, deployment["resources"])
    if ledger_path.is_file():
        for attempt in load_json(ledger_path)["attempts"]:
            if not completed_profile(attempt):
                continue
            command = [
                "import",
                "--database",
                database,
                "--profile-manifest",
                attempt["profile_manifest"],
                "--output-dir",
                _relative(group.output_dir / "database_import"),
            ]
            if attempt.get("cpu_service_cost"):
                command.extend(("--cpu-service-cost", attempt["cpu_service_cost"]))
                correction = load_json(require_repo_path(attempt["cpu_service_cost"]))
                if correction.get("measurement_window", {}).get("scope") == "all_controlled_requests":
                    command.append("--include-setup-requests")
            if deployment["resources"] is not None:
                command.extend(("--resource-environment", _relative(resource_path)))
            if _run_action("cost-db", *command):
                return False

    evidence = []
    for acquisition in acquisitions:
        if acquisition["operation"] != "eviction_locked_candidate" or acquisition["status"] != "cpu_primitive_measured":
            continue
        source = acquisition["report"]
        payload = load_json(require_repo_path(source))
        payload["operator_observations"] = locked_candidate_observations(payload, source, deployment)
        evidence.append(
            dict(
                capture="cpu_primitive:" + source,
                kind="cpu_primitive",
                manifest=source,
                deployment=deployment,
                payload=payload,
            )
        )
    if group.physical and group.physical.get("measurement_sources"):
        sources = sorted(group.physical["measurement_sources"])
        identity = "physical:" + ",".join(sources)
        evidence.append(
            dict(
                capture=identity, kind="physical_io", manifest=sources[0], deployment=deployment, payload=group.physical
            )
        )
        old = [value for value in request.get("io_evidence", []) if not value.startswith("physical:")]
        request["io_evidence"] = [*old, identity]
    if group.raw.get("control_calibrations"):
        controls = control_evidence(group.raw["control_calibrations"], deployment)
        evidence.extend(controls)
        # Control documents contribute observations to the database. Prediction
        # consumes trained operator models, never the raw document collection.
    if evidence:
        path = group.output_dir / "database_evidence.json"
        write_json(path, {"evidence": evidence})
        if _run_action(
            "cost-db",
            "import",
            "--database",
            database,
            "--evidence",
            _relative(path),
            "--output-dir",
            _relative(group.output_dir / "database_import"),
        ):
            return False

    return _train_group_model(group, request)


def _train_group_model(group: GroupRequest, request: dict[str, Any]) -> bool:
    """Train the declared shared model from admitted data, excluding this group's bases."""

    request["excluded_captures"] = sorted(
        set(request.get("excluded_captures", [])) | {source.run_id for source in group.sources}
    )
    request["excluded_manifests"] = sorted(
        set(request.get("excluded_manifests", [])) | {_relative(source.manifest_path) for source in group.sources}
    )
    path = group.output_dir / "cost_training.json"
    write_json(path, request)
    model_path = require_repo_path(group.raw["cost_model"])
    # Training can publish a usable subset and return 2 for other declared
    # models that still lack data. Execution, not package-wide completeness,
    # determines the next shared demand. A fresh temporary output distinguishes
    # that outcome from a failed command leaving an older package behind.
    with TemporaryDirectory(prefix="cost_training_", dir=model_path.parent) as directory:
        trained = Path(directory) / "cost_model.json"
        code = _run_action(
            "cost-db",
            "train",
            "--database",
            group.raw["cost_database"],
            "--request",
            _relative(path),
            "--output",
            _relative(trained),
        )
        if code not in {0, 2} or not trained.is_file():
            return False
        trained.replace(model_path)

    return True


def predict_group(
    group: GroupRequest,
    summary: dict[str, Any],
    *,
    model_run_jobs: int,
    diagnostics: str,
    experiment_reports: bool = False,
) -> None:
    """Consume a trained model; optional budgeted acquisition serves the whole group."""

    # The sampler imports group definitions; keep this cycle at orchestration.
    from .calibration.eviction_cpu import acquire_eviction_cpu

    attempted: set[str] = set()
    refreshed_database = False
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
        if not group.raw.get("cost_database") or not group.raw.get("cost_training"):
            summary["status"] = "acquisition_requires_database_and_training_request"
            return

        training = load_json(require_repo_path(group.raw["cost_training"]))
        needs, undeclared, corrections = declared_cost_demands(needs, training["models"])
        if undeclared:
            summary["undeclared_operator_models"] = undeclared
        if corrections:
            summary["cost_corrections_required"] = corrections
            summary["status"] = "cost_corrections_required"
            return
        if not needs:
            summary["status"] = "cost_model_definitions_required"
            return

        # An exported package may predate independently imported measurements.
        # Explicit automatic acquisition first reuses that database once; no
        # device work or target observations are needed for this refresh.
        if not refreshed_database:
            summary["database_refresh"] = "training"
            if not _train_group_model(group, training):
                summary["database_refresh"] = "failed"
                summary["status"] = "shared_training_incomplete"
                return

            refreshed_database = True
            summary["database_refresh"] = "completed"
            continue

        components = {need["component"] for need in needs}
        services = (components & SERVICE_SAMPLERS.keys()) - attempted
        operations = ["io_service"] if services else []
        operations.extend(pending_control_experiments(needs, attempted))
        phase_needed = any(component.startswith(("prefill/", "decode/")) for component in components)
        phase_needed |= bool(operator_requirements(needs, "allocator.preparation"))
        if phase_needed and "phase" not in attempted:
            operations.append("phase")

        acquisitions = []
        evidence_ready = False
        for operation in operations:
            attempted.update(services if operation == "io_service" else [operation])
            if operation == "io_service":
                result = capture_physical(group, dry_run=False, required_components=services)
                acquired = result["status"] == "physical_captured" or bool(result.get("completed_components"))
            elif operation == "eviction_locked_candidate":
                result = acquire_eviction_cpu(group, needs)
                # A completed measurement may predate a failed import/train.
                # Reuse still needs admission into the current database/model;
                # it does not consume another acquisition attempt.
                acquired = result["status"] == "cpu_primitive_measured"
            elif operation == "phase":
                result = capture_paired_calibration(group, materialize_fixed_calibration(group, phase_only=True))
                acquired = result["status"] == "paired_calibrated"
            else:
                result = prepare_control_calibration(group, operation, needs)
                acquired = result["status"] == "control_calibrated"

            acquisitions.append(dict(result, operation=operation))
            evidence_ready |= acquired
            group = GroupRequest.load(group.path)
        summary.setdefault("captures", []).extend(acquisitions)
        if not evidence_ready:
            return
        if not _import_acquired(group, acquisitions):
            summary["status"] = "shared_training_incomplete"
            return


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
