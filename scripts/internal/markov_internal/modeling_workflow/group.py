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

from ..common.io import load_json
from ..common.manifest import profile_labels
from ..common.paths import require_repo_path
from ..modeling.deployment import source_environment
from ..profiling.suite import profile_experiments
from .capture import PhysicalBudget, ProfileBudget
from ..modeling.cost.control import ControlMeasurement, control_calibrations, read_control_measurements
from ..modeling.source_dag.profiles import (
    ProfileRunRef,
    discover_profile_runs,
    extract_hicache_from_server_command,
)
from .types import TargetHiCacheConfig


if TYPE_CHECKING:
    from .physical_capture import PhysicalCaptureInputs


@dataclass(frozen=True)
class GroupRequest:
    path: Path
    raw: dict[str, Any]
    profile_suite: Path | None
    base_config: str
    workload_ids: tuple[str, ...]
    sources: tuple[ProfileRunRef, ...]
    targets: tuple[TargetHiCacheConfig, ...]
    budget: ProfileBudget
    output_dir: Path
    base_budget: ProfileBudget | None
    physical_budget: PhysicalBudget | None

    @classmethod
    def load(cls, path: Path) -> GroupRequest:
        path = require_repo_path(path)
        raw = load_json(path)
        allowed = {
            "profile_suite",
            "base_config",
            "workload_ids",
            "base_manifests",
            "target_configs",
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
                from .profile_capture import captured_base_manifests

                recovered = captured_base_manifests({**raw, "base_config": base}, suite, missing)
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
        if raw.get("cpu_service_capture_budget") is not None:
            ProfileBudget.parse(raw["cpu_service_capture_budget"])
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

        declared_targets = raw.get("target_configs")
        if not isinstance(declared_targets, list) or not declared_targets:
            raise ValueError(
                "target_configs requires policy objects, target JSON paths, or declared suite server names"
            )
        targets = []
        for target in declared_targets:
            if isinstance(target, dict):
                targets.append(TargetHiCacheConfig.from_raw(target))
            elif isinstance(target, str) and target in servers:
                policies = [extract_hicache_from_server_command(command) for command in servers[target]]
                fields = policies[0]
                if fields is None:
                    raise ValueError(f"target server has no usable HiCache configuration: {target}")
                if any(policy != fields for policy in policies[1:]):
                    raise ValueError(
                        f"target label has different experiment configurations; use an explicit target: {target}"
                    )
                targets.append(TargetHiCacheConfig.from_raw({"name": target, "hicache": fields}))
            elif isinstance(target, str):
                targets.append(TargetHiCacheConfig.load(Path(target)))
            else:
                raise ValueError("target configuration must be a policy object or path/name")
        if len({target.label for target in targets}) != len(targets):
            raise ValueError("target names must be unique within a group")
        raw = {**raw, "base_config": base, "workload_ids": list(workload_ids)}
        return cls(
            path=path,
            raw=raw,
            profile_suite=suite_path,
            base_config=base,
            workload_ids=workload_ids,
            sources=sources,
            targets=tuple(targets),
            budget=ProfileBudget.parse(raw.get("budget", dict(wall_seconds=0, server_starts=0, requests=0, tokens=0))),
            output_dir=require_repo_path(raw["output_dir"]),
            base_budget=base_budget,
            physical_budget=physical_budget,
        )

    @cached_property
    def sampling(self) -> PhysicalCaptureInputs:
        """Only deployment/domain inputs cross into the shared physical sampler."""
        from .physical_capture import PhysicalCaptureInputs

        return PhysicalCaptureInputs(
            capture=self.raw.get("physical_capture") or {},
            calibration=self.raw.get("physical_calibration"),
            resources=self.raw.get("resource_environment"),
            forced_token_bundle=self.raw.get("forced_token_bundle"),
            sources=self.sources,
            output_dir=self.output_dir,
            physical_budget=self.physical_budget,
        )

    @property
    def physical(self) -> dict[str, Any] | None:
        from .physical_capture import admitted_physical_calibration

        if self.missing_base_workloads:
            return admitted_physical_calibration(self.raw.get("physical_calibration"))

        return self.sampling.physical

    @cached_property
    def controls(self) -> tuple[ControlMeasurement, ...]:
        """Resolve independent control inputs for acquisition, not normal prediction.

        A fresh group after capture resolves the updated collection. Admission
        still rejects base evidence and incompatible runtime environments.
        """

        generated = self.output_dir / "control_calibrations.json"
        collection = control_calibrations(
            load_json(generated) if generated.exists() else {}, self.raw.get("control_calibrations", {})
        )
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
        from .profile_capture import profile_token_plan

        return profile_token_plan(source, self.raw.get("forced_token_bundle"))


def _admit_controls(group: GroupRequest, measurements: tuple[ControlMeasurement, ...]) -> None:
    """Reuse explicitly shared measurements, not per-target costs or old models.

    C++ validates operation semantics when consuming these measurements. This
    boundary checks the measurement sources and the same environment contract
    used for supplementary profiles; it does not certify execution coverage.
    """
    base_environment = (
        source_environment(group.sources[0], group.raw.get("resource_environment")) if group.sources else None
    )
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
                and source_environment(source, group.raw.get("resource_environment")) != base_environment
            ):
                raise ValueError(f"control calibration changed model, TP or runtime resources: {measurement.path}")
            admitted_manifests.add(manifest)
