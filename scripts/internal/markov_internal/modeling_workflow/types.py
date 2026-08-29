"""Core immutable contracts shared across modeling-workflow layers."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from .artifacts import ModelRunArtifacts
    from .io_model import HiCacheIoModel


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
class TargetHiCacheConfig:
    """Explicit target policy/capacity input for one prediction."""

    label: str
    fields: dict[str, Any]
    source_path: Path | None = None

    @classmethod
    def from_profile(cls, profile: ProfileRunRef) -> TargetHiCacheConfig:
        """Project an observed profile config into the prediction contract for scoring."""

        if profile.hicache_config is None:
            raise ValueError(f"profile has no HiCache config: {profile.label}")
        return cls(label=profile.config_id, fields=dict(profile.hicache_config), source_path=profile.config_path)

    def matches_source(self, source: ProfileRunRef) -> bool:
        """Compare policy/capacity values without relying on experiment identifiers."""

        if source.hicache_config is None:
            return False
        return {"enabled": True, **source.hicache_config} == {"enabled": True, **self.fields}


@dataclass(frozen=True)
class CacheStatePredictionRef:
    """Replay one source profile under an explicit target configuration."""

    source: ProfileRunRef
    target: TargetHiCacheConfig

    @property
    def input_id(self) -> str:
        """Return the shared workload input identity for this prediction."""

        return self.source.input_id

    @property
    def is_self(self) -> bool:
        """Return whether source and target configurations are identical."""

        return self.target.matches_source(self.source)

    @property
    def label(self) -> str:
        """Return the compact source-to-target prediction label."""

        return f"{self.input_id}/{self.source.config_id}->{self.target.label}"


@dataclass(frozen=True)
class ModelRunSpec:
    """One Direct HiCache prediction executed by the C++ DAG model."""

    run_id: str
    source_profile: ProfileRunRef
    target_config: TargetHiCacheConfig
    output_dir: Path
    prediction: CacheStatePredictionRef
    skip_reason: str = ""
    trace_threads: int = 1
    trace_file_threads: int = 1
    trace_channels: tuple[str, ...] = ()
    hicache_io_model: HiCacheIoModel | None = None

    @property
    def label(self) -> str:
        """Return the source-to-target prediction label."""

        return self.prediction.label


@dataclass(frozen=True)
class ModelRunResult:
    """Outcome of executing, skipping, or dry-running one spec."""

    spec: ModelRunSpec
    return_code: int
    elapsed_sec: float
    artifacts: ModelRunArtifacts
    skipped: bool = False
    dry_run: bool = False
    skip_reason: str = ""
    execution_error_tail: str = ""

    @property
    def ok(self) -> bool:
        """Return whether the spec ran successfully."""

        return not self.skipped and self.return_code == 0


@dataclass(frozen=True)
class ModelRunCounts:
    """Canonical model-run counts shared by progress and JSON summaries."""

    handled: int
    runnable: int
    usable: int
    errors: int
    skipped: int
    dry_run: int

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
        )
