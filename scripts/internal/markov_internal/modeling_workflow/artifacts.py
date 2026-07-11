"""Central artifact layout for workflow and per-model-run outputs."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

from ..common.io import load_json

if TYPE_CHECKING:
    from .types import ModelRunSpec


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
    def validations_dir(self) -> Path:
        """Return the root containing validation summaries and row artifacts."""

        return self.artifacts_dir / "validations"

    @property
    def plan_path(self) -> Path:
        """Return the normalized model-run plan artifact path."""

        return self.artifacts_dir / "model_run_plan.json"

    @property
    def preflight_summary_path(self) -> Path:
        """Return the workflow-facing preflight summary path."""

        return self.output_dir / "preflight_summary.json"

    @property
    def model_runs_summary_path(self) -> Path:
        """Return the aggregate model-run execution summary path."""

        return self.artifacts_dir / "model_runs_summary.json"

    @property
    def workflow_summary_path(self) -> Path:
        """Return the top-level workflow summary path."""

        return self.output_dir / "workflow_summary.json"

    def validation_summary_path(self, validation_name: str) -> Path:
        """Return the summary path owned by one registered validation."""

        return self.validations_dir / validation_name / "summary.json"

    def validation_row_path(self, validation_name: str, model_run_id: str) -> Path:
        """Return one validation's per-model-run diagnostic row path."""

        return self.validations_dir / validation_name / f"{model_run_id}.json"

    def model_run_dir(self, model_run_id: str) -> Path:
        """Return the isolated output directory for one normalized model run."""

        return self.model_runs_dir / model_run_id

    def ensure_base_dirs(self) -> None:
        """Create the stable top-level directory structure idempotently."""

        for path in (
            self.artifacts_dir,
            self.preflight_dir,
            self.model_runs_dir,
            self.validations_dir,
        ):
            path.mkdir(parents=True, exist_ok=True)


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
    def execution_json(self) -> Path:
        """Return the durable execution-status record path."""

        return self.output_dir / "execution.json"

    @property
    def prediction_json(self) -> Path:
        """Return the workflow-facing prediction path."""

        return self.output_dir / "prediction.json"

    @property
    def run_summary_json(self) -> Path:
        """Return the C++ backend run-summary path."""

        return self.output_dir / "run_summary.json"

    @property
    def validation_json(self) -> Path:
        """Return the container-runner validation summary path."""

        return self.output_dir / "validation.json"

    @property
    def model_summary_json(self) -> Path:
        """Return the Debug C++ module-summary path."""

        return self.output_dir / "model_summary.json"

    @property
    def dag_quality_json(self) -> Path:
        """Return the compact base-DAG quality artifact path."""

        return self.output_dir / "dag_quality.json"

    @property
    def dag_analysis_json(self) -> Path:
        """Return the detailed Debug DAG-analysis artifact path."""

        return self.output_dir / "dag_analysis.json"

    @property
    def dag_anchor_coverage_json(self) -> Path:
        """Return the DAG synchronization-anchor coverage path."""

        return self.output_dir / "dag_anchor_coverage.json"

    @property
    def dag_operation_visibility_json(self) -> Path:
        """Return the DAG operation-visibility diagnostic path."""

        return self.output_dir / "dag_operation_visibility.json"

    @property
    def predicted_state_trace_json(self) -> Path:
        """Return the predicted target cache-state transition trace path."""

        return self.output_dir / "predicted_target_cache_state_trace.json"

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
