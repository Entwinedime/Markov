"""Runtime options and shared state for the modeling workflow."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

from markov_internal.modeling.cost.hicache import HiCacheCostModels
from markov_internal.modeling.types import TargetHiCacheConfig


@dataclass(frozen=True)
class PredictionInputs:
    """Prepared file inputs for the public prediction call, shared read-only.

    Every source is an explicit manifest. CPU corrections belong to those
    sources, and cost_model is an independently prepared package, never a DB.
    """

    source_manifests: tuple[Path, ...]
    cost_model: Path
    cpu_service_costs: tuple[Path, ...] = ()
    resource_environment: dict[str, str] | None = None


class DiagnosticLevel(str, Enum):
    """Explicit retention policy for optional diagnostics and command logs."""

    OFF = "off"
    FULL = "full"

    @property
    def keep_debug_artifacts(self) -> bool:
        return self is DiagnosticLevel.FULL

    @property
    def failure_log_max_bytes(self) -> int:
        return 16 * 1024 * 1024 if self is DiagnosticLevel.FULL else 64 * 1024


@dataclass(frozen=True)
class WorkflowOptions:
    """Validated, repository-resolved options consumed by the workflow.

    CLI parsing remains outside this record so importing workflow modules never
    loads arguments or observes process-global command-line state.
    """

    source_manifests: tuple[Path, ...]
    target_configs: tuple[TargetHiCacheConfig, ...]
    output_dir: Path
    diagnostics: DiagnosticLevel
    cost_models: HiCacheCostModels
    dry_run: bool = False
    continue_on_error: bool = False
    trace_threads: int = 1
    trace_file_threads: int = 1
    model_run_jobs: int = 1
    cpu_service_costs: dict[Path, Path] = field(default_factory=dict)
    source_resources: dict[str, str] | None = None
    experiment_reports: bool = False
