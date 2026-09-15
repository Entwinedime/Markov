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
from ..common.paths import require_repo_path
from ..contracts.forced_token.bundle import resolve_forced_token_bundle_plan
from ..profiling.suite import matrix_entries
from .base_capture import captured_base_manifests, matching_base_attempts
from .physical_capture import captured_physical_declaration
from .planning.profile_runs import ProfileRunDiscovery, extract_hicache_from_server_command, parse_server_command_flags
from .types import ProfileRunRef, TargetHiCacheConfig


@dataclass(frozen=True)
class AcquisitionBudget:
    """Complete acquisition limits, including startup and failed attempts."""

    wall_seconds: float

    @classmethod
    def parse(cls, raw: dict[str, Any]) -> AcquisitionBudget:
        budget = cls(**raw)
        if any(not isinstance(value, (int, float)) or not 0 <= value < float("inf") for value in vars(budget).values()):
            raise ValueError("calibration budgets must be finite and non-negative")
        if any(not isinstance(value, int) or isinstance(value, bool) for key, value in vars(budget).items() if key != "wall_seconds"):
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
class CalibrationBudget(ProfileBudget):
    repeats: int

    @classmethod
    def parse(cls, raw: dict[str, Any]) -> CalibrationBudget:
        budget = super().parse(raw)
        if budget.repeats <= 0:
            raise ValueError("fixed calibration requires at least one repeat per endpoint")
        return budget


@dataclass(frozen=True)
class GroupRequest:
    path: Path
    raw: dict[str, Any]
    profile_suite: Path
    base_config: str
    workload_ids: tuple[str, ...]
    sources: tuple[ProfileRunRef, ...]
    targets: tuple[TargetHiCacheConfig, ...]
    physical: dict[str, Any] | None
    fixed_calibration_manifests: tuple[Path, ...]
    budget: CalibrationBudget
    output_dir: Path

    @classmethod
    def load(cls, path: Path) -> GroupRequest:
        path = require_repo_path(path)
        raw = load_json(path)
        allowed = {"profile_suite", "base_config", "workload_ids", "base_manifests", "target_configs",
                   "physical_calibration", "fixed_calibration_manifests", "budget", "output_dir",
                   "forced_token_bundle", "base_capture_budget", "physical_capture"}
        if set(raw) - allowed:
            raise ValueError(f"unknown group inputs (target observations are score-only): {sorted(set(raw) - allowed)}")
        suite_path = require_repo_path(raw["profile_suite"])
        suite = load_json(suite_path)
        if suite["framework"] != "sglang":
            raise ValueError("HiCache group preparation requires SGLang; other frameworks retain build-dag")
        servers = matrix_entries(suite["matrix"], "servers")
        inputs = matrix_entries(suite["matrix"], "inputs")
        base = str(raw["base_config"])
        workload_ids = tuple(sorted(set(raw["workload_ids"])))
        if base not in servers or not workload_ids or not set(workload_ids) <= inputs.keys():
            raise ValueError("base and non-empty workload set must be declared in the profiling suite")
        sources = tuple(ProfileRunDiscovery((), tuple(require_repo_path(p) for p in raw.get("base_manifests", []))).discover())
        recovered = captured_base_manifests(raw, suite, set(workload_ids) - {source.input_id for source in sources})
        sources += tuple(ProfileRunDiscovery((), tuple(recovered)).discover())
        if raw.get("base_capture_budget") is not None:
            ProfileBudget.parse(raw["base_capture_budget"])
        if (raw.get("physical_capture") or {}).get("budget") is not None:
            PhysicalBudget.parse(raw["physical_capture"]["budget"])
        if any(run.config_id != base or run.input_id not in workload_ids for run in sources):
            raise ValueError("base manifests must belong to this base and the declared workloads")
        if len({run.input_id for run in sources}) != len(sources):
            raise ValueError("provide one base manifest per workload; calibration repetitions are separate inputs")
        if len({json.dumps(run.hicache_config, sort_keys=True) for run in sources}) > 1:
            raise ValueError("base manifests must share one HiCache configuration")
        environments = [source_environment(run) for run in sources]
        if environments and any(value != environments[0] for value in environments[1:]):
            raise ValueError("base manifests must share model, TP and runtime resource settings")

        declared_targets = raw.get("target_configs")
        if not isinstance(declared_targets, list) or not declared_targets or any(
            not isinstance(value, str) or value not in servers for value in declared_targets
        ):
            raise ValueError("target_configs must name non-empty server declarations from profile_suite")
        target_ids = sorted(set(declared_targets))
        targets = []
        for target_id in target_ids:
            fields = extract_hicache_from_server_command(servers[target_id]["server"]["command"])
            if fields is None:
                raise ValueError(f"target server does not declare a usable HiCache configuration: {target_id}")
            targets.append(TargetHiCacheConfig(label=target_id, fields=fields))
        physical = admitted_physical_calibration(raw.get("physical_calibration"))
        fixed_manifests = tuple(require_repo_path(value) for value in raw.get("fixed_calibration_manifests", []))
        if len(set(fixed_manifests)) != len(fixed_manifests) or any(not value.is_file() for value in fixed_manifests):
            raise ValueError("fixed calibration manifests must be distinct existing files")
        group = cls(path, raw, suite_path, base, workload_ids, sources, tuple(targets), physical,
                    fixed_manifests, CalibrationBudget.parse(raw["budget"]), require_repo_path(raw["output_dir"]))
        if physical is None and not group.missing_base_workloads:
            physical = admitted_physical_calibration(captured_physical_declaration(group))
        return replace(group, physical=physical)

    @property
    def missing_base_workloads(self) -> tuple[str, ...]:
        return tuple(sorted(set(self.workload_ids) - {source.input_id for source in self.sources}))

    @property
    def base_budget(self) -> ProfileBudget | None:
        raw = self.raw.get("base_capture_budget")
        return ProfileBudget.parse(raw) if raw is not None else None

    @property
    def physical_budget(self) -> PhysicalBudget | None:
        raw = (self.raw.get("physical_capture") or {}).get("budget")
        return PhysicalBudget.parse(raw) if raw is not None else None

    def token_plan(self, workload: str) -> Path | None:
        bundle = self.raw.get("forced_token_bundle")
        if not bundle:
            admitted = {source.manifest_path for source in self.sources if source.input_id == workload}
            rows = matching_base_attempts(self.raw, load_json(self.profile_suite), workload)
            row = next((row for row in reversed(rows) if row["status"] == "completed" and row["mode"] == "replay"
                        and require_repo_path(row["profile_manifest"]) in admitted), None)
            bundle = row["forced_token_bundle"] if row else None
        return Path(resolve_forced_token_bundle_plan(require_repo_path(bundle), workload).plan_path) if bundle else None


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
    # Presence of control/phase in an old bundle cannot silently grant permission
    # to consume those labels. They are rebuilt from this group's observations.
    admitted = {
        key: report[key]
        for key in ("kv_geometry", "service_models", "resource_lanes", "storage_batch_pages")
    }
    admitted["measurement_scope"] = report.get("measurement_scope") or {}
    admitted["measurement_sources"] = list(declaration["measurement_sources"])
    admitted["measurement_description"] = str(declaration["measurement_description"])
    return admitted


def source_environment(source: ProfileRunRef) -> dict[str, Any]:
    """Runtime dimensions in which a group's observations may be shared."""

    flags = parse_server_command_flags(source.run_dir / "server_cmd.txt")
    flags.setdefault("base_gpu_id", "0")
    flags.setdefault("gpu_id_step", "1")
    config = load_json(source.config_path)
    names = ("model_path", "tp_size", "dtype", "kv_cache_dtype", "attention_backend", "sampling_backend",
             "hicache_io_backend", "hicache_mem_layout", "hicache_storage_backend", "numa_node", "base_gpu_id", "gpu_id_step",
             "max_running_requests", "chunked_prefill_size", "disable_cuda_graph")
    return {"server": {name: flags.get(name) for name in names},
            "env": {key: value for key, value in config.get("env", {}).items()
                    if key != "SGLANG_HICACHE_FILE_BACKEND_STORAGE_DIR"}}
