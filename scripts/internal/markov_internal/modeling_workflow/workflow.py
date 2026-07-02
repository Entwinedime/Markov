"""统一建模 workflow 编排器。"""

from __future__ import annotations

from dataclasses import dataclass

from .artifacts import WorkflowArtifactLayout
from .context import WorkflowContext, WorkflowOptions
from .execution import ModelRunExecutor
from .planning.plan_io import write_model_run_plan
from .planning.profile_runs import ProfileRunDiscovery, RunSelector
from .planning.specs import ModelRunPlanner
from .preflight import PreflightRunner
from .progress import WorkflowProgressReporter
from .reporting import write_workflow_summary
from .types import ProfileRunRef, ValidationSummary
from .validations import ValidationRequest, validation_by_name


@dataclass(frozen=True)
class WorkflowRunner:
    """串联 preflight、C++ 建模运行和 Python validation 分析。"""

    options: WorkflowOptions

    def run(self) -> int:
        """执行完整 workflow。"""

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
        self._report_plan(context, len(specs))
        results = ModelRunExecutor(context, specs).run()
        validation_summaries = self._analyze_validations(context, validations, specs, results)
        write_workflow_summary(
            context,
            runs=runs,
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

    def _report_plan(self, context: WorkflowContext, spec_count: int) -> None:
        context.reporter.start_stage("plan", 1, f"model-runs {spec_count}", unit="item").finish(
            "OK",
            f"{spec_count} model runs | validations {','.join(self.options.validations)}",
        )

    def _analyze_validations(
        self,
        context: WorkflowContext,
        validations: list[ValidationRequest],
        specs: list,
        results: dict,
    ) -> list[ValidationSummary]:
        return [validation.analyze(context, specs, results) for validation in validations]
