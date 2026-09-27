"""Core immutable contracts shared across modeling-workflow layers."""

from __future__ import annotations

from dataclasses import dataclass
from functools import cached_property
from pathlib import Path
from typing import Any

from ..modeling.workload import WorkloadWindow, discover_workload_window


@dataclass(frozen=True)
class ProfileRunRef:
    """One concrete profiling run selected as workflow input."""

    manifest_path: Path
    run_dir: Path
    config_path: Path
    run_id: str
    config_id: str
    input_id: str
    python_probe_files: tuple[Path, ...]
    hicache_config: dict[str, Any] | None = None

    @cached_property
    def workload_window(self) -> WorkloadWindow | None:
        """Read this immutable capture's window lazily, shared by its targets.

        A new workflow creates new source objects; nothing is cached on disk.
        Failed reads propagate without caching a fabricated window.
        """

        return discover_workload_window({}, self.manifest_path)

    @property
    def label(self) -> str:
        """Return the compact input/config label used in progress and artifacts."""

        return f"{self.input_id}/{self.config_id}"


@dataclass(frozen=True)
class TargetHiCacheConfig:
    """Target policy/capacity input with prefetch_policy resolved at admission."""

    label: str
    fields: dict[str, Any]

    def matches_source(self, source: ProfileRunRef) -> bool:
        """Compare policy/capacity values without relying on experiment identifiers."""

        if source.hicache_config is None:
            return False
        return {"enabled": True, **source.hicache_config} == {"enabled": True, **self.fields}


@dataclass(frozen=True)
class ModelRunSpec:
    """One source/target task; shared execution settings belong to WorkflowOptions."""

    run_id: str
    output_dir: Path
    source: ProfileRunRef
    target: TargetHiCacheConfig
    skip_reason: str = ""
    cpu_service_cost: Path | None = None

    @property
    def label(self) -> str:
        """Return the source-to-target prediction label."""

        return f"{self.source.input_id}/{self.source.config_id}->{self.target.label}"


@dataclass(frozen=True)
class ModelRunResult:
    """Outcome of executing, skipping, or dry-running one spec."""

    spec: ModelRunSpec
    return_code: int
    skip_reason: str = ""
    missing_costs: tuple[dict[str, Any], ...] = ()

    @property
    def ok(self) -> bool:
        """Command success only; DAG completion is checked from its execution report."""

        return not self.skip_reason and self.return_code == 0
