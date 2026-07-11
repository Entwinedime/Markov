"""Core immutable contracts shared across modeling-workflow layers."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from .artifacts import ModelRunArtifacts


class ModelOutputRequirement(str, Enum):
    """Semantic artifact families required from one C++ invocation."""

    DAG_ANALYSIS = "dag_analysis"
    HICACHE_VALIDATION = "hicache_validation"


@dataclass(frozen=True)
class ProfileRunRef:
    """One concrete profiling run selected as workflow input."""

    manifest_path: Path
    run_dir: Path
    config_path: Path
    run_id: str
    config_id: str
    input_id: str
    input_class: str
    python_probe_files: tuple[Path, ...]
    hicache_config: dict[str, Any] | None = None

    @property
    def label(self) -> str:
        """Return the compact input/config label used in progress and artifacts."""

        return f"{self.input_id}/{self.config_id}"


@dataclass(frozen=True)
class CacheStatePredictionRef:
    """Replay a source profile under a target configuration and oracle."""

    source: ProfileRunRef
    target: ProfileRunRef

    @property
    def input_id(self) -> str:
        """Return the shared workload input identity for this prediction."""

        return self.source.input_id

    @property
    def is_self(self) -> bool:
        """Return whether source and target configurations are identical."""

        return self.source.config_id == self.target.config_id

    @property
    def label(self) -> str:
        """Return the compact source-to-target prediction label."""

        return f"{self.input_id}/{self.source.config_id}->{self.target.config_id}"


@dataclass(frozen=True)
class ModelRunSpec:
    """One normalized C++ execution after validation-request merging."""

    run_id: str
    mode: str
    source_profile: ProfileRunRef
    target_profile: ProfileRunRef | None
    output_requirements: frozenset[ModelOutputRequirement]
    validation_requests: tuple[str, ...]
    output_dir: Path
    prediction: CacheStatePredictionRef | None = None
    skip_reason: str = ""
    trace_threads: int = 1
    trace_file_threads: int = 1
    trace_channels: tuple[str, ...] = ()
    page_key_mode: str = "strip_scope"

    @property
    def label(self) -> str:
        """Return the prediction label or faithful source-profile label."""

        if self.prediction is not None:
            return self.prediction.label
        return self.source_profile.label

    @property
    def output_requirement_names(self) -> tuple[str, ...]:
        """Return deterministic schema names for requested artifact families."""

        return tuple(sorted(requirement.value for requirement in self.output_requirements))


@dataclass(frozen=True)
class ModelRunResult:
    """Outcome of executing, reusing, skipping, or dry-running one spec."""

    spec: ModelRunSpec
    return_code: int
    elapsed_sec: float
    artifacts: ModelRunArtifacts
    skipped: bool = False
    dry_run: bool = False
    reused: bool = False
    skip_reason: str = ""
    execution_error_tail: str = ""

    @property
    def ok(self) -> bool:
        """Return whether the spec actually ran or was reused successfully."""

        return not self.skipped and self.return_code == 0


@dataclass
class ValidationSummary:
    """Compact workflow-facing result from one validation object."""

    name: str
    status: str
    selected_run_count: int
    ready_count: int = 0
    exact_count: int | None = None
    skipped_count: int = 0
    blocker_counts: dict[str, int] = field(default_factory=dict)
    artifact_paths: dict[str, str] = field(default_factory=dict)
    payload: dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class ModelRunCounts:
    """Canonical model-run counts shared by progress and JSON summaries."""

    handled: int
    runnable: int
    usable: int
    errors: int
    skipped: int
    dry_run: int
    reused: int

    @classmethod
    def from_results(cls, results: list[ModelRunResult] | tuple[ModelRunResult, ...]) -> ModelRunCounts:
        """Derive mutually consistent progress counters from completed results."""

        runnable = [result for result in results if not result.skipped]
        return cls(
            handled=len(results),
            runnable=len(runnable),
            usable=sum(result.return_code == 0 for result in runnable),
            errors=sum(result.return_code != 0 for result in runnable),
            skipped=sum(result.skipped for result in results),
            dry_run=sum(result.dry_run for result in results),
            reused=sum(result.reused for result in results),
        )

    def as_payload(self) -> dict[str, int]:
        """Serialize counters under stable workflow-summary field names."""

        return {
            "handled_count": self.handled,
            "runnable_count": self.runnable,
            "usable_count": self.usable,
            "error_count": self.errors,
            "skipped_count": self.skipped,
            "dry_run_count": self.dry_run,
            "reused_count": self.reused,
        }
