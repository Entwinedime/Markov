"""Runtime options and shared state for the modeling workflow."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from .artifacts import WorkflowArtifactLayout
from .progress import WorkflowProgressReporter
from .types import ProfileRunRef


@dataclass(frozen=True)
class WorkflowOptions:
    """Validated, repository-resolved options consumed by the workflow.

    CLI parsing remains outside this record so importing workflow modules never
    loads arguments or observes process-global command-line state.
    """

    profile_run_dirs: tuple[Path, ...]
    manifests: tuple[Path, ...]
    output_dir: Path
    validations: tuple[str, ...]
    input_ids: frozenset[str]
    config_ids: frozenset[str]
    source_config_ids: frozenset[str]
    target_config_ids: frozenset[str]
    prediction_scope: frozenset[str]
    dry_run: bool = False
    force: bool = False
    continue_on_error: bool = False
    max_predictions: int = 0
    page_key_mode: str = "strip_scope"
    sample_limit: int = 20
    trace_threads: int = 1
    trace_file_threads: int = 1
    model_run_jobs: int = 1


@dataclass
class WorkflowContext:
    """State shared by planning, preflight checks, execution, and validation."""

    options: WorkflowOptions
    runs: list[ProfileRunRef]
    artifacts: WorkflowArtifactLayout
    reporter: WorkflowProgressReporter
