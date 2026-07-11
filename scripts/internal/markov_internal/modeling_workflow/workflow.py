"""Top-level orchestration for the host-side modeling workflow."""

from __future__ import annotations

from dataclasses import dataclass

from .artifacts import WorkflowArtifactLayout
from .context import WorkflowContext, WorkflowOptions
from .execution.model_executor import ModelRunExecutor
from .planning.plan_io import write_model_run_plan
from .planning.profile_runs import ProfileRunDiscovery, RunSelector
from .planning.specs import ModelRunPlanner
from .preflight import PreflightRunner
from .progress import WorkflowProgressReporter
from .reporting.workflow_summary import write_workflow_summary
from .types import ModelRunResult, ModelRunSpec, ProfileRunRef, ValidationSummary
from .validations.registry import ValidationRequest, validation_by_name


@dataclass(frozen=True)
class WorkflowRunner:
    """Run preflight, C++ modeling cells, and Python validation analysis.

    The runner owns sequencing only. Validation objects define their required
    C++ cells and analyze the resulting artifacts; the runner does not encode
    validation-specific fixture or comparison behavior.
    """

    options: WorkflowOptions

    def run(self) -> int:
        """Execute the complete workflow and write its aggregate summary."""

        artifacts = WorkflowArtifactLayout(self.options.output_dir)
        artifacts.ensure_base_dirs()
        runs = self._selected_runs()
        validations = self._validations()
        context = WorkflowContext(
            options=self.options,
            runs=runs,
            artifacts=artifacts,
            reporter=WorkflowProgressReporter(),
        )

        preflight_report = PreflightRunner(context, self._preflight_checks(validations)).run()
        specs = ModelRunPlanner(context, artifacts, preflight_report).build(validations)
        write_model_run_plan(artifacts, runs, specs, selected_validations=self.options.validations)
        results = ModelRunExecutor(context, specs).run()
        validation_summaries = self._analyze_validations(context, validations, specs, results)
        write_workflow_summary(
            context,
            specs=specs,
            preflight_report=preflight_report,
            results=results,
            validation_summaries=validation_summaries,
        )
        return 0

    def _selected_runs(self) -> list[ProfileRunRef]:
        runs = ProfileRunDiscovery(self.options.profile_run_dirs, self.options.manifests).discover()
        runs = RunSelector(
            input_ids=self.options.input_ids,
            config_ids=self.options.config_ids,
            source_config_ids=self.options.source_config_ids,
            target_config_ids=self.options.target_config_ids,
        ).filter(runs)
        if not runs:
            raise SystemExit("No profile manifests matched the requested workflow.")
        return runs

    def _validations(self) -> list[ValidationRequest]:
        return [validation_by_name(name) for name in self.options.validations]

    def _preflight_checks(self, validations: list[ValidationRequest]) -> list[type]:
        return [check for validation in validations for check in validation.preflight_checks()]

    def _analyze_validations(
        self,
        context: WorkflowContext,
        validations: list[ValidationRequest],
        specs: list[ModelRunSpec],
        results: dict[str, ModelRunResult],
    ) -> list[ValidationSummary]:
        return [validation.analyze(context, specs, results) for validation in validations]
