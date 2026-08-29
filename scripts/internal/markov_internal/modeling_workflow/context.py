"""Runtime options and shared state for the modeling workflow."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from .artifacts import ArtifactPolicy, WorkflowArtifactLayout
from .io_model import HiCacheIoModel
from .progress import WorkflowProgressReporter
from .types import ProfileRunRef, TargetHiCacheConfig


@dataclass(frozen=True)
class WorkflowOptions:
    """Validated, repository-resolved options consumed by the workflow.

    CLI parsing remains outside this record so importing workflow modules never
    loads arguments or observes process-global command-line state.
    """

    profile_run_dirs: tuple[Path, ...]
    source_manifests: tuple[Path, ...]
    target_configs: tuple[TargetHiCacheConfig, ...]
    output_dir: Path
    input_ids: frozenset[str]
    config_ids: frozenset[str]
    source_config_ids: frozenset[str]
    target_config_ids: frozenset[str]
    artifact_policy: ArtifactPolicy
    dry_run: bool = False
    continue_on_error: bool = False
    max_predictions: int = 0
    trace_threads: int = 1
    trace_file_threads: int = 1
    model_run_jobs: int = 1
    hicache_io_model: HiCacheIoModel | None = None
    base_io_models: tuple[tuple[str, HiCacheIoModel], ...] = ()
    oracle_scores: tuple[tuple[str, Path, Path], ...] = ()
    evaluation: bool = False


@dataclass
class WorkflowContext:
    """State shared by planning, preflight checks, execution, and validation."""

    options: WorkflowOptions
    runs: list[ProfileRunRef]
    artifacts: WorkflowArtifactLayout
    reporter: WorkflowProgressReporter
