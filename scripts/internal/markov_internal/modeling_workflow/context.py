"""Runtime options and shared state for the modeling workflow."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

from ..common.io import load_json
from ..common.paths import require_repo_path
from .io_model import HiCacheIoModel
from .types import TargetHiCacheConfig


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
    dry_run: bool = False
    continue_on_error: bool = False
    trace_threads: int = 1
    trace_file_threads: int = 1
    model_run_jobs: int = 1
    hicache_io_model: HiCacheIoModel | None = None
    cpu_service_costs: dict[Path, Path] = field(default_factory=dict)


def cpu_service_inputs(paths: list[Path], sources: tuple[Path, ...]) -> dict[Path, Path]:
    """Bind explicit service files to selected sources, never prediction targets."""

    inputs = {}
    for path in paths:
        path = require_repo_path(path)
        source = require_repo_path(load_json(path)["source_manifest"]).resolve()
        if source not in sources:
            raise ValueError(f"CPU service input does not belong to a selected source: {path}")
        if source in inputs:
            raise ValueError(f"Multiple CPU service inputs for the same source: {source}")
        inputs[source] = path
    return inputs
