"""Optional recovery of shared cost evidence after an actual prediction gap."""

from __future__ import annotations

from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any

from ..common.io import load_json, write_json
from ..common.paths import ROOT_DIR, repo_relative_path, require_repo_path
from ..common.process import run_command
from ..modeling.cost.models import ModelSpec
from ..modeling.cost.package import control_evidence
from ..modeling.deployment import source_environment
from .calibration.eviction_cpu import acquire_eviction_cpu, locked_candidate_observations
from .capture import completed_profile
from .control_acquisition import (
    capture_paired_calibration,
    declared_cost_demands,
    operator_requirements,
    pending_control_experiments,
    prepare_control_calibration,
)
from .fixed_calibration import materialize_fixed_calibration
from .group import GroupRequest
from .physical_capture import SERVICE_SAMPLERS, capture_physical


def _relative(path: Path) -> str:
    return str(repo_relative_path(path))


def _run_action(action: str, *arguments: str) -> int:
    return run_command([str(ROOT_DIR / "scripts/model.sh"), action, *arguments]).returncode


def _import_acquired(
    group: GroupRequest, acquisitions: list[dict[str, Any]], request: dict[str, Any]
) -> bool:
    """Admit new independent captures, then explicitly retrain one shared model.

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
        sources = sorted(group.physical["measurement_sources"])
        identity = "physical:" + ",".join(sources)
        evidence.append(
            dict(
                capture=identity, kind="physical_io", manifest=sources[0], deployment=deployment, payload=group.physical
            )
        )
        old = [value for value in request.get("io_evidence", []) if not value.startswith("physical:")]
        request["io_evidence"] = [*old, identity]
    # Resolve these inputs only for independent import. Normal prediction
    # consumes the trained package and does not open calibration documents.
    evidence.extend(control_evidence(group.controls, deployment))
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


class CostAcquisition:
    """One group's recovery session, independent of prediction execution.

    Each advance either refreshes existing independent data, captures a new
    shared experiment batch, or returns a concrete stopping reason. The
    preparation workflow owns prediction and decides when another run occurs.
    Attempts remain bounded by existing cumulative ledgers and never use scores.
    """

    def __init__(self, group: GroupRequest) -> None:
        self.group = group
        self.attempted: set[str] = set()
        self.database_refreshed = False
        self.training: dict[str, Any] | None = None
        self.specifications: list[ModelSpec] = []

    def advance(self, needs: list[dict[str, Any]]) -> dict[str, Any]:
        """Prepare evidence for the next prediction, without executing it."""

        group = self.group
        if not group.raw.get("cost_database") or not group.raw.get("cost_training"):
            return {"status": "acquisition_requires_database_and_training_request"}

        if self.training is None:
            self.training = load_json(require_repo_path(group.raw["cost_training"]))
            self.specifications = [ModelSpec.from_raw(model) for model in self.training["models"]]
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
            if not _train_group_model(group, training):
                return dict(outcome, status="shared_training_incomplete", database_refresh="failed")

            self.database_refreshed = True
            return dict(outcome, status="database_refreshed", database_refresh="completed")

        components = {need["component"] for need in needs}
        services = (components & SERVICE_SAMPLERS.keys()) - self.attempted
        operations = ["io_service"] if services else []
        operations.extend(pending_control_experiments(needs, self.attempted))
        phase_needed = any(component.startswith(("prefill/", "decode/")) for component in components)
        phase_needed |= bool(operator_requirements(needs, "allocator.preparation"))
        if phase_needed and "phase" not in self.attempted:
            operations.append("phase")

        captures = []
        evidence_ready = False
        for operation in operations:
            self.attempted.update(services if operation == "io_service" else [operation])
            if operation == "io_service":
                result = capture_physical(group, dry_run=False, required_components=services)
                acquired = result["status"] == "physical_captured" or bool(result.get("completed_components"))
            elif operation == "eviction_locked_candidate":
                result = acquire_eviction_cpu(group, needs)
                acquired = result["status"] == "cpu_primitive_measured"
            elif operation == "phase":
                result = capture_paired_calibration(group, materialize_fixed_calibration(group, phase_only=True))
                acquired = result["status"] == "paired_calibrated"
            else:
                result = prepare_control_calibration(group, operation, needs)
                acquired = result["status"] == "control_calibrated"

            captures.append(dict(result, operation=operation))
            evidence_ready |= acquired
            group = GroupRequest.load(group.path)

        self.group = group
        if not evidence_ready:
            return dict(outcome, status="acquisition_incomplete", captures=captures)
        if not _import_acquired(group, captures, training):
            return dict(outcome, status="shared_training_incomplete", captures=captures)

        return dict(outcome, status="costs_prepared", captures=captures)
