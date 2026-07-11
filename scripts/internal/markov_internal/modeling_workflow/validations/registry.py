"""Validation interfaces, reusable templates, and lazy type registry."""

from __future__ import annotations

from abc import ABC, abstractmethod
from importlib import import_module
from typing import TYPE_CHECKING, Any

from ...common.io import write_json
from ..planning.profile_runs import PredictionMatrixBuilder
from ..planning.specs import ModelRunRequest
from ..types import ModelOutputRequirement, ModelRunResult, ModelRunSpec, ValidationSummary

if TYPE_CHECKING:
    from ..context import WorkflowContext
    from ..preflight import PreflightCheck


class ValidationRequest(ABC):
    """Python-side contract for one independently selectable validation."""

    name: str

    @abstractmethod
    def preflight_checks(self) -> tuple[type[PreflightCheck], ...]:
        """Return profile-input checks required before planning model runs."""

    @abstractmethod
    def build_model_run_requests(self, context: WorkflowContext) -> list[ModelRunRequest]:
        """Return semantic C++ executions required by this validation."""

    @abstractmethod
    def analyze(
        self,
        context: WorkflowContext,
        specs: list[ModelRunSpec],
        results: dict[str, ModelRunResult],
    ) -> ValidationSummary:
        """Read C++ artifacts, persist validation results, and summarize them."""

    def selected_specs(self, specs: list[ModelRunSpec]) -> list[ModelRunSpec]:
        """Select merged model runs that satisfy this validation request."""

        return [spec for spec in specs if self.name in spec.validation_requests]


class RowValidation(ValidationRequest):
    """Template for validations represented by one row per model run."""

    progress_detail: str
    progress_unit = "run"

    def analyze(
        self,
        context: WorkflowContext,
        specs: list[ModelRunSpec],
        results: dict[str, ModelRunResult],
    ) -> ValidationSummary:
        """Execute the shared model-result to row to summary pipeline."""

        rows: list[dict[str, Any]] = []
        selected = self.selected_specs(specs)
        progress = context.reporter.start_stage(
            self.name,
            len(selected),
            self.progress_detail,
            unit=self.progress_unit,
        )
        for spec in selected:
            row = self.build_row(context, results[spec.run_id])
            rows.append(row)
            write_json(context.artifacts.validation_row_path(self.name, spec.run_id), row)
            progress.advance(self.running_metrics(rows))

        summary = self.build_summary(context, rows)
        write_json(context.artifacts.validation_summary_path(self.name), summary)
        progress.finish(str(summary["status"]), self.summary_text(summary))
        return self.validation_summary(context, rows, summary)

    @abstractmethod
    def build_row(self, context: WorkflowContext, result: ModelRunResult) -> dict[str, Any]:
        """Build one persisted validation row from a model-run result."""

    @abstractmethod
    def running_metrics(self, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """Return compact metrics for the transient progress line."""

    @abstractmethod
    def build_summary(self, context: WorkflowContext, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """Aggregate validation rows into the persisted summary payload."""

    @abstractmethod
    def summary_text(self, summary: dict[str, Any]) -> str:
        """Render the durable final progress-line summary."""

    @abstractmethod
    def validation_summary(
        self,
        context: WorkflowContext,
        rows: list[dict[str, Any]],
        summary: dict[str, Any],
    ) -> ValidationSummary:
        """Convert validation-specific data to the workflow summary contract."""


class PredictionValidation(ValidationRequest):
    """Mixin that requests cache-state cells from one prediction matrix."""

    cache_state_output_requirements: frozenset[ModelOutputRequirement]

    def build_model_run_requests(self, context: WorkflowContext) -> list[ModelRunRequest]:
        """Build one cache-state request for each selected matrix cell."""

        predictions = PredictionMatrixBuilder(
            runs=context.runs,
            source_config_ids=context.options.source_config_ids,
            target_config_ids=context.options.target_config_ids,
            prediction_scope=context.options.prediction_scope,
            max_predictions=context.options.max_predictions,
        ).build()
        return [
            ModelRunRequest(
                mode="cache_state",
                source_profile=prediction.source,
                target_profile=prediction.target,
                output_requirements=self.cache_state_output_requirements,
                validation_name=self.name,
                prediction=prediction,
            )
            for prediction in predictions
        ]


VALIDATION_TYPES = {
    "base_dag": (".base_dag.request", "BaseDagValidation"),
    "final_dag": (".final_dag.request", "FinalDagValidation"),
    "hicache_dag_mapping": (".hicache.dag_mapping", "HiCacheDagMappingValidation"),
    "hicache_final_state": (".hicache.final_state", "HiCacheFinalStateValidation"),
    "hicache_transition": (".hicache.transition.request", "HiCacheTransitionValidation"),
}


def validation_names() -> tuple[str, ...]:
    """Return stable validation names accepted by the CLI."""

    return tuple(VALIDATION_TYPES)


def validation_by_name(name: str) -> ValidationRequest:
    """Construct a registered validation while preserving lazy imports."""

    try:
        module_name, class_name = VALIDATION_TYPES[name]
    except KeyError as error:
        raise KeyError(f"unknown validation: {name}") from error
    validation_type = getattr(import_module(module_name, package=__package__), class_name)
    validation = validation_type()
    if not isinstance(validation, ValidationRequest):
        raise TypeError(f"Registered validation {name!r} does not implement ValidationRequest")
    return validation


def count_blockers(rows: list[dict[str, Any]], key: str) -> dict[str, int]:
    """Count scalar or list-valued blockers across validation rows."""

    counts: dict[str, int] = {}
    for row in rows:
        values = row.get(key)
        if isinstance(values, list):
            for value in values:
                text = str(value)
                counts[text] = counts.get(text, 0) + 1
        elif values:
            text = str(values)
            counts[text] = counts.get(text, 0) + 1
    return dict(sorted(counts.items()))


def readiness_status(total: int, error_count: int, ready_count: int) -> str:
    """Return the common status for validations governed by readiness only."""

    if total == 0:
        return "EMPTY"
    if error_count:
        return "ERROR"
    return "OK" if ready_count == total else "CHECK"
