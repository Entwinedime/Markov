"""One base's explicit inputs, shared calibration budget and source-only boundary.

No directory-wide matrix discovery or target score path belongs to this contract.
The host prepare workflow and container requirement scanner share this input.
"""

from __future__ import annotations

from dataclasses import dataclass
from functools import cached_property
import json
from pathlib import Path
from typing import Any, TYPE_CHECKING

from markov_internal.common.io import load_json
from markov_internal.common.manifest import profile_labels
from markov_internal.common.paths import require_repo_path
from markov_internal.modeling.deployment import source_environment
from markov_internal.profiling.suite import profile_experiments
from markov_internal.preparation.capture import PhysicalBudget, ProfileBudget
from markov_internal.preparation.cpu_service import CpuServiceInputs
from markov_internal.modeling.cost.control import ControlMeasurement, control_calibrations, read_control_measurements
from markov_internal.modeling.source_dag.profiles import (
    ProfileRunRef,
    discover_profile_runs,
)


if TYPE_CHECKING:
    from markov_internal.preparation.physical_capture import PhysicalCaptureInputs


@dataclass(frozen=True)
class ScenarioRequest:
    """Parsed application inputs, never a mutable JSON preparation ledger.

    Nested declarations are read-only inputs owned by this request. Preparation
    selects a new cost_model with replace(), without rewriting the input file.
    Capture and result state belong to their workflows, not these declarations.
    """

    path: Path
    profile_suite: Path | None
    base_config: str
    workload_ids: tuple[str, ...]
    sources: tuple[ProfileRunRef, ...]
    budget: ProfileBudget
    output_dir: Path
    base_budget: ProfileBudget | None
    physical_budget: PhysicalBudget | None
    cost_model: Path | None
    cost_database: Path | None
    cost_training: Path | None
    automatic_cost_acquisition: bool
    resource_environment: dict[str, str] | None
    forced_token_bundle: str | None
    cpu_service: CpuServiceInputs
    physical_capture: dict[str, Any]
    physical_calibration: dict[str, Any] | None
    control_inputs: dict[str, Any]

    @classmethod
    def load(cls, path: Path) -> ScenarioRequest:
        path = require_repo_path(path)
        raw = load_json(path)
        allowed = {
            "profile_suite",
            "base_config",
            "workload_ids",
            "base_manifests",
            "target_configs",  # The combined public command resolves these in the application only.
            "physical_calibration",
            "budget",
            "output_dir",
            "forced_token_bundle",
            "base_capture_budget",
            "physical_capture",
            "cpu_service_pairs",
            "cpu_service_capture_budget",
            "cpu_service_costs",
            "control_calibrations",
            "cost_model",
            "cost_database",
            "cost_training",
            "automatic_cost_acquisition",
            "resource_environment",
        }
        if set(raw) - allowed:
            raise ValueError(f"unknown group inputs (target observations are score-only): {sorted(set(raw) - allowed)}")
        output_dir = require_repo_path(raw["output_dir"])
        sources = tuple(discover_profile_runs(tuple(require_repo_path(p) for p in raw.get("base_manifests", []))))
        suite_path = require_repo_path(raw["profile_suite"]) if raw.get("profile_suite") else None
        base = str(raw.get("base_config") or (sources[0].config_id if sources else ""))
        workload_ids = tuple(sorted(set(raw.get("workload_ids", [source.input_id for source in sources]))))
        servers = {}
        if suite_path is not None:
            suite = load_json(suite_path)
            if suite["framework"] != "sglang":
                raise ValueError("HiCache group preparation requires SGLang; other frameworks retain build-dag")
            declared = set()
            for experiment in profile_experiments(suite):
                config_id, input_id = profile_labels(experiment)
                declared.add((config_id, input_id))
                servers.setdefault(config_id, []).append(experiment["server"]["command"])

            # A single base declaration already selects its workloads. Matrix
            # inputs still require an explicit base; never choose the first row.
            if not base and len(servers) == 1:
                base = next(iter(servers))
            if "workload_ids" not in raw and not workload_ids:
                workload_ids = tuple(sorted(workload for config, workload in declared if config == base and workload))

            if not base or not workload_ids or any((base, workload) not in declared for workload in workload_ids):
                raise ValueError(
                    "select one declared base and its workloads; "
                    "a profiling input with one base can supply the selection directly"
                )
            missing = set(workload_ids) - {source.input_id for source in sources}
            if missing:
                # Only missing bases need the capture ledger and planners.
                from markov_internal.preparation.profile_capture import captured_base_manifests

                recovered = captured_base_manifests(
                    output_dir, base, suite, missing, forced_token_bundle=raw.get("forced_token_bundle")
                )
                sources += tuple(discover_profile_runs(tuple(recovered)))
        elif not sources or set(workload_ids) != {source.input_id for source in sources}:
            raise ValueError("without profile_suite, provide a base manifest for every requested workload")
        if any(source.hicache_config is None for source in sources):
            raise ValueError(
                "HiCache preparation requires base profiles with a HiCache configuration; "
                "use build-dag for ordinary profiles"
            )

        base_budget = raw.get("base_capture_budget")
        base_budget = ProfileBudget.parse(base_budget) if base_budget is not None else None
        physical_budget = (raw.get("physical_capture") or {}).get("budget")
        physical_budget = PhysicalBudget.parse(physical_budget) if physical_budget is not None else None
        if any(run.config_id != base or run.input_id not in workload_ids for run in sources):
            raise ValueError("base manifests must belong to this base and the declared workloads")
        if len({run.input_id for run in sources}) != len(sources):
            raise ValueError("provide one base manifest per workload; calibration repetitions are separate inputs")
        if len({json.dumps(run.hicache_config, sort_keys=True) for run in sources}) > 1:
            raise ValueError("base manifests must share one HiCache configuration")
        environments = [source_environment(run, raw.get("resource_environment")) for run in sources]
        if environments and any(value != environments[0] for value in environments[1:]):
            raise ValueError("base manifests must share model, TP and runtime resource settings")

        return cls(
            path=path,
            profile_suite=suite_path,
            base_config=base,
            workload_ids=workload_ids,
            sources=sources,
            budget=ProfileBudget.parse(raw.get("budget", dict(wall_seconds=0, server_starts=0, requests=0, tokens=0))),
            output_dir=output_dir,
            base_budget=base_budget,
            physical_budget=physical_budget,
            cost_model=require_repo_path(raw["cost_model"]) if raw.get("cost_model") else None,
            cost_database=require_repo_path(raw["cost_database"]) if raw.get("cost_database") else None,
            cost_training=require_repo_path(raw["cost_training"]) if raw.get("cost_training") else None,
            automatic_cost_acquisition=bool(raw.get("automatic_cost_acquisition", False)),
            resource_environment=raw.get("resource_environment"),
            forced_token_bundle=raw.get("forced_token_bundle"),
            cpu_service=CpuServiceInputs.parse(raw),
            physical_capture=raw.get("physical_capture") or {},
            physical_calibration=raw.get("physical_calibration"),
            control_inputs=raw.get("control_calibrations", {}),
        )

    @cached_property
    def sampling(self) -> PhysicalCaptureInputs:
        """Only deployment/domain inputs cross into the shared physical sampler."""
        from markov_internal.preparation.physical_capture import PhysicalCaptureInputs

        return PhysicalCaptureInputs(
            capture=self.physical_capture,
            calibration=self.physical_calibration,
            resources=self.resource_environment,
            forced_token_bundle=self.forced_token_bundle,
            sources=self.sources,
            output_dir=self.output_dir,
            physical_budget=self.physical_budget,
        )

    @property
    def physical(self) -> dict[str, Any] | None:
        from markov_internal.preparation.physical_capture import admitted_physical_calibration

        if self.missing_base_workloads:
            return admitted_physical_calibration(self.physical_calibration)

        return self.sampling.physical

    @cached_property
    def controls(self) -> tuple[ControlMeasurement, ...]:
        """Resolve independent control inputs for acquisition, not normal prediction.

        A fresh group after capture resolves the updated collection. Admission
        still rejects base evidence and incompatible runtime environments.
        """

        generated = self.output_dir / "control_calibrations.json"
        collection = control_calibrations(load_json(generated) if generated.exists() else {}, self.control_inputs)
        measurements = read_control_measurements(collection)
        _admit_controls(self, measurements)
        return measurements

    @property
    def missing_base_workloads(self) -> tuple[str, ...]:
        return tuple(sorted(set(self.workload_ids) - {source.input_id for source in self.sources}))

    def token_plan(self, workload: str) -> Path | None:
        """Use an explicit bundle or the selected base's actual replay input."""

        source = next((source for source in self.sources if source.input_id == workload), None)
        if source is None:
            return None

        # Token replay inputs are needed by optional acquisition, not prediction.
        from markov_internal.preparation.profile_capture import profile_token_plan

        return profile_token_plan(source, self.forced_token_bundle)


def _admit_controls(group: ScenarioRequest, measurements: tuple[ControlMeasurement, ...]) -> None:
    """Reuse explicitly shared measurements, not per-target costs or old models.

    C++ validates operation semantics when consuming these measurements. This
    boundary checks the measurement sources and the same environment contract
    used for supplementary profiles; it does not certify execution coverage.
    """
    base_environment = source_environment(group.sources[0], group.resource_environment) if group.sources else None
    base_manifests = {source.manifest_path for source in group.sources}
    admitted_manifests: set[Path] = set()
    for measurement in measurements:
        document = measurement.document
        if measurement.selection[0] == "prefetch_wait":
            policy = measurement.selection[1]
            if (policy == "best_effort") != (document["source_policy"] == "best_effort"):
                raise ValueError("shared prefetch calibration does not match its declared program")

        source = measurement.source
        manifest = source.manifest_path
        if manifest in base_manifests:
            raise ValueError("base measurements are not independent control calibration")
        if manifest not in admitted_manifests:
            if (
                base_environment is not None
                and source_environment(source, group.resource_environment) != base_environment
            ):
                raise ValueError(f"control calibration changed model, TP or runtime resources: {measurement.path}")
            admitted_manifests.add(manifest)
