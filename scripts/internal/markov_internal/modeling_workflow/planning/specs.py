"""Planning and deduplication of semantic C++ model-run requests."""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING

from ...common.naming import safe_slug
from ..artifacts import WorkflowArtifactLayout
from ..types import (
    CacheStatePredictionRef,
    ModelRunSpec,
    ProfileRunRef,
    TargetHiCacheConfig,
)

if TYPE_CHECKING:
    from ..context import WorkflowContext
    from ..io_model import HiCacheIoModel


@dataclass(frozen=True)
class ModelRunRequest:
    """One source-to-target Direct HiCache prediction request."""

    source_profile: ProfileRunRef
    target_config: TargetHiCacheConfig
    prediction: CacheStatePredictionRef
    hicache_io_model: HiCacheIoModel | None = None


@dataclass(frozen=True)
class ModelRunPlanner:
    """Translate Direct prediction requests into C++ execution specs."""

    context: WorkflowContext
    artifacts: WorkflowArtifactLayout
    preflight_report: dict[str, object]

    def build(self, requests: list[ModelRunRequest]) -> list[ModelRunSpec]:
        """Build the final plan after applying preflight skip decisions."""

        skip_policy = PreflightSkipPolicy.from_report(self.preflight_report)
        specs = [self._spec(request, skip_policy) for request in requests]
        run_ids = [spec.run_id for spec in specs]
        if len(run_ids) != len(set(run_ids)):
            raise ValueError("Direct prediction plan produced duplicate source/target/workload cells")
        return sorted(specs, key=lambda spec: spec.run_id)

    def _spec(self, request: ModelRunRequest, skip_policy: PreflightSkipPolicy) -> ModelRunSpec:
        io_model = request.hicache_io_model or self.context.options.hicache_io_model
        run_id = model_run_id(request.prediction)
        return ModelRunSpec(
            run_id=run_id,
            source_profile=request.source_profile,
            target_config=request.target_config,
            output_dir=self.artifacts.model_run_dir(run_id),
            prediction=request.prediction,
            skip_reason=skip_policy.reason_for(request, io_model_available=io_model is not None),
            trace_threads=self.context.options.trace_threads,
            trace_file_threads=self.context.options.trace_file_threads,
            trace_channels=("torch", "ld_preload", "python_probe"),
            hicache_io_model=io_model,
        )


@dataclass(frozen=True)
class PreflightSkipPolicy:
    """Translate preflight evidence into stable per-cell skip reasons."""

    full_dag_by_run: dict[str, dict[str, object]]
    hicache_by_run: dict[str, dict[str, object]]

    @classmethod
    def from_report(cls, report: dict[str, object]) -> PreflightSkipPolicy:
        """Index shared preflight detail rows used by deterministic skip decisions."""

        checks = report.get("checks") if isinstance(report.get("checks"), dict) else {}
        full_dag = (
            checks.get("full_dag_trace_channels") if isinstance(checks.get("full_dag_trace_channels"), dict) else {}
        )
        hicache = checks.get("hicache_state_inputs") if isinstance(checks.get("hicache_state_inputs"), dict) else {}
        return cls(
            full_dag_by_run=index_rows(full_dag.get("rows")),
            hicache_by_run=index_rows(hicache.get("runs")),
        )

    def reason_for(
        self,
        request: ModelRunRequest,
        *,
        io_model_available: bool,
    ) -> str:
        """Return an empty string or a stable machine-readable skip reason."""

        full_dag_reason = self._full_dag_reason(request.source_profile)
        if full_dag_reason:
            return full_dag_reason
        if not io_model_available:
            return "missing_hicache_io_model"
        return self._hicache_prediction_reason(request.source_profile)

    def _full_dag_reason(self, profile: ProfileRunRef) -> str:
        """Return the full-source trace blocker for one profile."""

        row = self.full_dag_by_run.get(profile.run_id)
        if not isinstance(row, dict):
            return "preflight_missing:full_dag_trace_channels"
        if row.get("full_trace_ready") is not True:
            errors = row.get("artifact_errors") if isinstance(row.get("artifact_errors"), list) else []
            if errors:
                return "full_dag_trace_not_ready:" + ",".join(str(error) for error in errors)
            return "full_dag_trace_not_ready"
        return ""

    def _hicache_prediction_reason(
        self,
        source: ProfileRunRef,
    ) -> str:
        source_preflight = self.hicache_by_run.get(source.run_id)
        return _hicache_readiness_reason(source_preflight)


def _hicache_readiness_reason(
    source_preflight: dict[str, object] | None,
) -> str:
    """Return source readiness blockers before scope-specific checks."""

    if isinstance(source_preflight, dict) and source_preflight.get("workflow_input_ready") is not True:
        return "source_workflow_input_not_ready"
    return ""


def index_rows(value: object) -> dict[str, dict[str, object]]:
    """Index well-formed preflight detail rows by run identifier."""

    if not isinstance(value, list):
        return {}
    return {str(row.get("run_id")): row for row in value if isinstance(row, dict) and row.get("run_id") is not None}


def model_run_id(prediction: CacheStatePredictionRef) -> str:
    """Build a readable Direct HiCache prediction identifier."""

    return "__".join(("hicache", safe_slug(prediction.input_id), prediction_config_slug(prediction)))


def prediction_config_slug(prediction: CacheStatePredictionRef) -> str:
    """Build the source-to-target configuration component of a path."""

    return f"{safe_slug(prediction.source.config_id)}_to_{safe_slug(prediction.target.label)}"
