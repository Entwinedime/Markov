"""Central artifact layout for workflow and per-model-run outputs."""

from __future__ import annotations

from collections.abc import Iterable
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import TYPE_CHECKING, Any

from ..common.io import load_json

if TYPE_CHECKING:
    from .types import ModelRunSpec


class DiagnosticLevel(str, Enum):
    """User-facing amount of optional diagnostic computation and retention."""

    OFF = "off"
    FULL = "full"


@dataclass(frozen=True)
class ArtifactPolicy:
    """One typed policy shared by planning, execution, and validation.

    Explicitly selected validations may still require Debug-backend evidence
    when optional diagnostics are off.  That evidence is a validation input,
    not permission to run unrelated diagnostics.
    """

    diagnostics: DiagnosticLevel = DiagnosticLevel.OFF

    @classmethod
    def from_value(cls, value: str | DiagnosticLevel) -> ArtifactPolicy:
        """Build the policy once from a validated CLI value."""

        return cls(diagnostics=DiagnosticLevel(value))

    @property
    def keep_debug_artifacts(self) -> bool:
        """Return whether detailed rows, successful logs, and C++ details remain."""

        return self.diagnostics is DiagnosticLevel.FULL

    @property
    def failure_log_max_bytes(self) -> int:
        """Return the maximum retained bytes for one failed model command."""

        if self.diagnostics is DiagnosticLevel.FULL:
            return 16 * 1024 * 1024
        return 64 * 1024

@dataclass(frozen=True)
class WorkflowArtifactLayout:
    """Derive every workflow-level path from one output directory."""

    output_dir: Path

    @property
    def artifacts_dir(self) -> Path:
        """Return the workflow's durable supporting-artifact directory."""

        return self.output_dir / "artifacts"

    @property
    def preflight_dir(self) -> Path:
        """Return the directory containing per-check preflight evidence."""

        return self.artifacts_dir / "preflight"

    @property
    def model_runs_dir(self) -> Path:
        """Return the root containing one directory per normalized model run."""

        return self.output_dir / "model_runs"

    @property
    def debug_rows_dir(self) -> Path:
        """Return the optional per-cell Debug row directory."""

        return self.artifacts_dir / "debug_rows"

    @property
    def plan_path(self) -> Path:
        """Return the normalized model-run plan artifact path."""

        return self.artifacts_dir / "model_run_plan.json"

    @property
    def preflight_summary_path(self) -> Path:
        """Return the workflow-facing preflight summary path."""

        return self.output_dir / "preflight_summary.json"

    @property
    def workflow_summary_path(self) -> Path:
        """Return the top-level workflow summary path."""

        return self.output_dir / "workflow_summary.json"

    def debug_row_path(self, model_run_id: str) -> Path:
        """Return one optional per-cell Debug row path."""

        return self.debug_rows_dir / f"{model_run_id}.json"

    def model_run_dir(self, model_run_id: str) -> Path:
        """Return the isolated output directory for one normalized model run."""

        return self.model_runs_dir / model_run_id

    def ensure_base_dirs(self) -> None:
        """Create only the workflow root; artifact writers own subdirectories."""

        self.output_dir.mkdir(parents=True, exist_ok=True)

@dataclass(frozen=True)
class ModelRunArtifacts:
    """Typed path index for artifacts produced by one C++ model run."""

    output_dir: Path

    @property
    def model_log(self) -> Path:
        """Return the private combined stdout/stderr log path."""

        return self.output_dir / "model.log"

    @property
    def runner_config_json(self) -> Path:
        """Return the self-contained Python runner-config path."""

        return self.output_dir / "runner_config.json"

    @property
    def cpp_model_config_json(self) -> Path:
        """Return the C++ business-config path referenced by the runner config."""

        return self.output_dir / "cpp_model_config.json"

    @property
    def run_summary_json(self) -> Path:
        """Return the C++ backend run-summary path."""

        return self.output_dir / "run_summary.json"

    @property
    def model_summary_json(self) -> Path:
        """Return the Debug C++ module-summary path."""

        return self.output_dir / "model_summary.json"

    @property
    def debug_details(self) -> tuple[Path, ...]:
        """Return large C++ details pruned after the prediction summary."""

        return (self.model_summary_json,)

    def load_if_present(self, path: Path) -> dict[str, Any]:
        """Load an optional JSON object, returning an empty mapping otherwise."""

        if not path.is_file():
            return {}
        payload = load_json(path)
        return payload if isinstance(payload, dict) else {}

    @classmethod
    def from_spec(cls, spec: ModelRunSpec) -> ModelRunArtifacts:
        """Bind artifact paths to the output directory owned by a model spec."""

        return cls(spec.output_dir)


def prune_debug_details(
    model_runs: Iterable[ModelRunArtifacts],
    policy: ArtifactPolicy,
) -> None:
    """Remove C++ details after the prediction summary unless Debug retention is enabled."""

    if policy.keep_debug_artifacts:
        return

    for artifacts in model_runs:
        for path in artifacts.debug_details:
            path.unlink(missing_ok=True)
