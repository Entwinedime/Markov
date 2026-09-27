"""One base's explicit inputs, shared calibration budget and source-only boundary.

No directory-wide matrix discovery or target score path belongs to this contract.
The host prepare workflow and container requirement scanner share this input.
"""

from __future__ import annotations

from dataclasses import dataclass, replace
import json
from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.manifest import profile_labels
from ..common.paths import require_repo_path
from ..contracts.forced_token.bundle import resolve_forced_token_bundle_plan
from ..contracts.forced_token.plan import load_forced_token_plan
from ..profiling.suite import expand_suite
from ..workload_template.cli import parse_workload_command
from .base_capture import captured_base_manifests
from .capture import CAPTURE_CLEANUP_RESERVE_SEC
from .control_calibrations import control_calibrations
from .physical_capture import captured_physical_declaration, platform_inputs
from .planning.profile_runs import (
    discover_profile_runs,
    extract_hicache_from_server_command,
    parse_server_command_flags,
)
from .planning.target_configs import load_target_config, parse_target_config
from .types import ProfileRunRef, TargetHiCacheConfig


@dataclass(frozen=True)
class AcquisitionBudget:
    """Complete acquisition limits, including startup and failed attempts."""

    wall_seconds: float

    def available_for(self, usage: dict[str, Any], **increments: int) -> tuple[float, list[str]]:
        """Return remaining wall seconds and exceeded limits for one planned attempt.

        Usage includes previous failures. Wall time retains the container cleanup
        reserve; other limits permit an attempt that exactly uses the remaining budget.
        """

        remaining = self.wall_seconds - usage["wall_seconds"]
        limits = [key for key, count in increments.items() if usage[key] + count > getattr(self, key)]
        if remaining <= CAPTURE_CLEANUP_RESERVE_SEC:
            limits.append("wall_seconds")
        return remaining, limits

    @classmethod
    def parse(cls, raw: dict[str, Any]) -> AcquisitionBudget:
        budget = cls(**raw)
        if any(not isinstance(value, (int, float)) or not 0 <= value < float("inf") for value in vars(budget).values()):
            raise ValueError("calibration budgets must be finite and non-negative")
        if any(
            not isinstance(value, int) or isinstance(value, bool)
            for key, value in vars(budget).items()
            if key != "wall_seconds"
        ):
            raise ValueError("calibration counts must be integers")
        return budget


@dataclass(frozen=True)
class ProfileBudget(AcquisitionBudget):
    server_starts: int
    requests: int
    tokens: int


@dataclass(frozen=True)
class PhysicalBudget(AcquisitionBudget):
    container_starts: int
    logical_io_bytes: int


@dataclass(frozen=True)
class GroupRequest:
    path: Path
    raw: dict[str, Any]
    profile_suite: Path | None
    base_config: str
    workload_ids: tuple[str, ...]
    sources: tuple[ProfileRunRef, ...]
    targets: tuple[TargetHiCacheConfig, ...]
    physical: dict[str, Any] | None
    fixed_calibration_manifests: tuple[Path, ...]
    budget: ProfileBudget
    output_dir: Path
    control_sources: dict[str, dict[str, str]]
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
            "fixed_calibration_manifests",
            "budget",
            "output_dir",
            "forced_token_bundle",
            "base_capture_budget",
            "physical_capture",
            "cpu_service_pairs",
            "cpu_service_capture_budget",
            "cpu_service_costs",
            "control_calibrations",
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
            for experiment in expand_suite(suite):
                config_id, input_id = profile_labels(experiment)
                declared.add((config_id, input_id))
                servers.setdefault(config_id, []).append(experiment["server"]["command"])
            if not base or not workload_ids or any((base, workload) not in declared for workload in workload_ids):
                raise ValueError("each base/workload must identify a declared profiling experiment")
            recovered = captured_base_manifests(
                {**raw, "base_config": base}, suite, set(workload_ids) - {source.input_id for source in sources}
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
        environments = [source_environment(run) for run in sources]
        if environments and any(not environments_match(value, environments[0]) for value in environments[1:]):
            raise ValueError("base manifests must share model, TP and runtime resource settings")

        declared_targets = raw.get("target_configs")
        if not isinstance(declared_targets, list) or not declared_targets:
            raise ValueError(
                "target_configs requires policy objects, target JSON paths, or declared suite server names"
            )
        targets = []
        for target in declared_targets:
            if isinstance(target, dict):
                targets.append(parse_target_config(target))
            elif isinstance(target, str) and target in servers:
                policies = [extract_hicache_from_server_command(command) for command in servers[target]]
                fields = policies[0]
                if fields is None:
                    raise ValueError(f"target server has no usable HiCache configuration: {target}")
                if any(policy != fields for policy in policies[1:]):
                    raise ValueError(
                        f"target label has different experiment configurations; use an explicit target: {target}"
                    )
                targets.append(parse_target_config({"name": target, "hicache": fields}))
            elif isinstance(target, str):
                targets.append(load_target_config(Path(target)))
            else:
                raise ValueError("target configuration must be a policy object or path/name")
        if len({target.label for target in targets}) != len(targets):
            raise ValueError("target names must be unique within a group")
        physical = admitted_physical_calibration(raw.get("physical_calibration"))
        fixed_manifests = tuple(require_repo_path(value) for value in raw.get("fixed_calibration_manifests", []))
        if len(set(fixed_manifests)) != len(fixed_manifests) or any(not value.is_file() for value in fixed_manifests):
            raise ValueError("fixed calibration manifests must be distinct existing files")
        raw = {**raw, "base_config": base, "workload_ids": list(workload_ids)}
        generated_controls = require_repo_path(raw["output_dir"]) / "control_calibrations.json"
        raw["control_calibrations"] = control_calibrations(
            load_json(generated_controls) if generated_controls.exists() else {},
            raw.get("control_calibrations", {}),
        )
        group = cls(
            path,
            raw,
            suite_path,
            base,
            workload_ids,
            sources,
            tuple(targets),
            physical,
            fixed_manifests,
            ProfileBudget.parse(raw.get("budget", dict(wall_seconds=0, server_starts=0, requests=0, tokens=0))),
            require_repo_path(raw["output_dir"]),
            {},
            base_budget,
            physical_budget,
        )
        if not group.missing_base_workloads:
            physical = (
                admitted_physical_calibration(captured_physical_declaration(group))
                or physical
                or platform_inputs(group)
            )
        return replace(
            group,
            physical=physical,
            control_sources=_control_sources(group, environments[0] if environments else None),
        )

    @property
    def missing_base_workloads(self) -> tuple[str, ...]:
        return tuple(sorted(set(self.workload_ids) - {source.input_id for source in self.sources}))

    def token_plan(self, workload: str) -> Path | None:
        """Use an explicit bundle or the selected base's actual replay input."""

        bundle = self.raw.get("forced_token_bundle")
        if bundle:
            return resolve_forced_token_bundle_plan(require_repo_path(bundle), workload)

        source = next((source for source in self.sources if source.input_id == workload), None)
        if source is None:
            return None
        args = parse_workload_command(load_json(source.config_path).get("bench", {}).get("command"))
        if args is None or args.forced_token_mode != "replay":
            return None

        path = require_repo_path(args.forced_token_plan)
        if load_forced_token_plan(path).get("workload_id") != workload:
            raise ValueError(f"base token plan belongs to another workload: {workload}")
        return path


def admitted_physical_calibration(declaration: dict[str, Any] | None) -> dict[str, Any] | None:
    """Admit explicitly sourced hardware curves, never embedded runtime/control."""

    if declaration is None:
        return None
    if not declaration.get("measurement_sources") or not declaration.get("measurement_description"):
        raise ValueError("physical calibration requires original measurement sources and their scope")
    for source in declaration["measurement_sources"]:
        if not require_repo_path(source).is_file():
            raise ValueError(f"physical calibration source is unavailable: {source}")
    report = load_json(require_repo_path(declaration["report"]))
    for field in ("target_workload_trace_used", "target_e2e_used"):
        if report.get(field) is not False:
            raise ValueError(f"physical calibration must declare {field}=false before any cost projection")
    # Presence of control/phase in an old bundle cannot silently grant permission
    # to consume those labels. They are rebuilt from this group's observations.
    admitted = {key: report[key] for key in ("kv_geometry", "service_models", "resource_lanes", "storage_batch_pages")}
    admitted["measurement_scope"] = report.get("measurement_scope") or {}
    admitted["measurement_sources"] = list(declaration["measurement_sources"])
    admitted["measurement_description"] = str(declaration["measurement_description"])
    for field in ("target_workload_trace_used", "target_e2e_used"):
        admitted[field] = report[field]
    return admitted


def _control_sources(group: GroupRequest, base_environment: dict[str, Any] | None) -> dict[str, dict[str, str]]:
    """Reuse explicitly shared measurements, not per-target costs or old models.

    C++ validates operation semantics when consuming these measurements. This
    boundary checks the measurement sources and the same environment contract
    used for supplementary profiles; it does not certify execution coverage.
    """
    collection = group.raw["control_calibrations"]
    paths = set(collection["prefetch_wait"].values())
    paths.update(path for policies in collection["write_host"].values() for path in policies.values())
    paths.update(value for key, value in collection.items() if key not in {"prefetch_wait", "write_host"})
    evidence: dict[str, dict[str, str]] = {}
    base_manifests = {source.manifest_path for source in group.sources}
    admitted_manifests: set[Path] = set()
    for path in sorted(paths):
        document = load_json(require_repo_path(path))
        for policy, wait_path in collection["prefetch_wait"].items():
            if wait_path == path and (policy == "best_effort") != (document["source_policy"] == "best_effort"):
                raise ValueError("shared prefetch calibration does not match its declared program")

        manifest = require_repo_path(document["source_manifest"])
        if manifest in base_manifests:
            raise ValueError("base measurements are not independent control calibration")
        if manifest not in admitted_manifests:
            (source,) = discover_profile_runs((manifest,))
            if base_environment is not None and not environments_match(source_environment(source), base_environment):
                raise ValueError(f"control calibration changed model, TP or runtime resources: {path}")
            admitted_manifests.add(manifest)
        evidence[path] = {
            "source_manifest": document["source_manifest"],
            "cost_basis": document.get("cost_basis", "source_profile_uncorrected"),
        }
    return evidence


def environments_match(first: dict, second: dict) -> bool:
    def comparable(value):
        env = dict(value.get("env", {}))
        if "SGLANG_STEP_TIMING_DIR" in env:
            env["SGLANG_STEP_TIMING_DIR"] = bool(env["SGLANG_STEP_TIMING_DIR"])
        return {**value, "env": env}

    return comparable(first) == comparable(second)


def source_environment(source: ProfileRunRef) -> dict[str, Any]:
    """Runtime dimensions in which a group's observations may be shared."""

    flags = parse_server_command_flags(source.run_dir / "server_cmd.txt")
    flags.setdefault("tp_size", "1")
    flags.setdefault("base_gpu_id", "0")
    flags.setdefault("gpu_id_step", "1")
    config = load_json(source.config_path)
    names = (
        "model_path",
        "tp_size",
        "dtype",
        "kv_cache_dtype",
        "attention_backend",
        "sampling_backend",
        "hicache_io_backend",
        "hicache_mem_layout",
        "hicache_storage_backend",
        "numa_node",
        "base_gpu_id",
        "gpu_id_step",
        "max_running_requests",
        "chunked_prefill_size",
        "disable_cuda_graph",
    )
    return {
        "server": {name: flags.get(name) for name in names},
        "env": {
            key: value
            for key, value in config.get("env", {}).items()
            if key != "SGLANG_HICACHE_FILE_BACKEND_STORAGE_DIR"
        },
    }
