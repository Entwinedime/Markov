"""Optional recovery of shared cost evidence after an actual prediction gap."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ..common.io import load_json, write_json
from ..common.paths import ROOT_DIR, repo_relative_path, require_repo_path
from ..common.process import run_command
from ..modeling.cost.database import CostDatabase
from ..modeling.cost.models import ModelSpec, parse_model_specs
from ..modeling.cost.package import control_evidence, physical_evidence
from ..modeling.deployment import source_environment
from .calibration.eviction_cpu import acquire_eviction_cpu, locked_candidate_observations
from .capture import completed_profile
from .cost_preparation import train_group_cost_model
from .control_acquisition import (
    capture_paired_calibration,
    declared_cost_demands,
    operator_requirements,
    plan_control_captures,
    prepare_control_calibration,
)
from .fixed_calibration import materialize_fixed_calibration
from .group import GroupRequest
from .physical_capture import SERVICE_SAMPLERS, capture_physical


def _relative(path: Path) -> str:
    return str(repo_relative_path(path))


def _run_action(action: str, *arguments: str) -> int:
    return run_command([str(ROOT_DIR / "scripts/model.sh"), action, *arguments]).returncode


def _import_acquired(group: GroupRequest, acquisitions: list[dict[str, Any]], request: dict[str, Any]) -> bool:
    """Admit new independent captures for the next offline training step.

    The offline request excludes every selected base. Acquisition never uses
    scores, target traces or per-cell fitted corrections.
    """

    database = group.raw["cost_database"]
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
        physical = physical_evidence(group.physical, deployment)
        evidence.append(physical)
        old = [value for value in request.get("io_evidence", []) if not value.startswith("physical:")]
        request["io_evidence"] = [*old, physical["capture"]]
    # Resolve these inputs only for independent import. Normal prediction
    # consumes the trained package and does not open calibration documents.
    database = CostDatabase(require_repo_path(group.raw["cost_database"]))
    evidence.extend(control_evidence(group.controls, deployment, database))
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

    return True


class CostAcquisition:
    """One group's recovery session, independent of prediction execution.

    Each advance either refreshes existing independent data, captures a new
    shared experiment batch, or returns a concrete stopping reason. The
    preparation workflow owns prediction and decides when another run occurs.
    Attempts remain bounded by existing cumulative ledgers and never use scores.
    """

    def __init__(self, group: GroupRequest, *, database_refreshed: bool = False) -> None:
        self.group = group
        self.attempted: set[str] = set()
        self.database_refreshed = database_refreshed
        self.training: dict[str, Any] | None = None
        self.specifications: list[ModelSpec] = []

    def advance(self, needs: list[dict[str, Any]]) -> dict[str, Any]:
        """Prepare evidence for the next prediction, without executing it."""

        group = self.group
        if not group.raw.get("cost_database") or not group.raw.get("cost_training"):
            return {"status": "acquisition_requires_database_and_training_request"}

        if self.training is None:
            self.training = load_json(require_repo_path(group.raw["cost_training"]))
            self.specifications = parse_model_specs(self.training.get("models", []))
        training = self.training
        needs, undeclared, corrections = declared_cost_demands(needs, self.specifications)
        outcome: dict[str, Any] = {}
        if undeclared:
            outcome["undeclared_operator_models"] = undeclared
        if corrections:
            return dict(outcome, status="cost_corrections_required", cost_corrections_required=corrections)
        if not needs:
            return dict(outcome, status="cost_model_definitions_required")

        # Refresh an older package from existing independent measurements once,
        # before deciding that a device experiment is necessary.
        if not self.database_refreshed:
            trained = train_group_cost_model(group, training)
            if trained is None:
                return dict(outcome, status="shared_training_incomplete", database_refresh="failed")

            self.group = trained
            self.database_refreshed = True
            return dict(outcome, status="database_refreshed", database_refresh="completed")

        components = {need["component"] for need in needs}
        services = (components & SERVICE_SAMPLERS.keys()) - self.attempted
        operations = ["io_service"] if services else []
        controls = plan_control_captures(needs, self.attempted)
        control_captures = {capture.experiment: capture for capture in controls}
        operations.extend(control_captures)
        phase_needed = any(component.startswith(("prefill/", "decode/")) for component in components)
        phase_needed |= bool(operator_requirements(needs, "allocator.preparation"))
        if phase_needed and "phase" not in self.attempted:
            operations.append("phase")

        captures = []
        evidence_ready = False
        for operation in operations:
            self.attempted.update(services if operation == "io_service" else [operation])
            if operation == "io_service":
                result = capture_physical(group.sampling, dry_run=False, required_components=services)
                acquired = result["status"] == "physical_captured" or bool(result.get("completed_components"))
            elif operation == "eviction_locked_candidate":
                result = acquire_eviction_cpu(group.sampling, needs)
                acquired = result["status"] == "cpu_primitive_measured"
            elif operation == "phase":
                result = capture_paired_calibration(group, materialize_fixed_calibration(group, phase_only=True))
                acquired = result["status"] == "paired_calibrated"
            else:
                result = prepare_control_calibration(group, control_captures[operation])
                acquired = result["status"] == "control_calibrated"

            captures.append(dict(result, operation=operation))
            evidence_ready |= acquired
            group = GroupRequest.load(group.path)

        self.group = group
        if not evidence_ready:
            return dict(outcome, status="acquisition_incomplete", captures=captures)
        if not _import_acquired(group, captures, training):
            return dict(outcome, status="shared_training_incomplete", captures=captures)

        trained = train_group_cost_model(group, training)
        if trained is None:
            return dict(outcome, status="shared_training_incomplete", captures=captures)

        self.group = trained
        return dict(outcome, status="costs_prepared", captures=captures)
