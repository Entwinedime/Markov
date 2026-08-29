"""Top-level orchestration for the host-side modeling workflow."""

from __future__ import annotations

from dataclasses import dataclass

from .artifacts import WorkflowArtifactLayout, prune_debug_details
from .context import WorkflowContext, WorkflowOptions
from .evaluation.hicache import HiCacheEvaluationRequest
from .execution.model_executor import ModelRunExecutor
from .planning.plan_io import write_model_run_plan
from .planning.profile_runs import ProfileRunDiscovery, RunSelector
from .planning.specs import ModelRunPlanner
from .preflight import PreflightRunner
from .prediction.hicache import HiCachePredictionRequest
from .progress import WorkflowProgressReporter
from .reporting.workflow_summary import write_workflow_summary
from .types import ProfileRunRef


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
        prediction = HiCacheEvaluationRequest() if self.options.evaluation else HiCachePredictionRequest()
        context = WorkflowContext(
            options=self.options,
            runs=runs,
            artifacts=artifacts,
            reporter=WorkflowProgressReporter(),
        )

        preflight_report = PreflightRunner(context, list(prediction.preflight_checks())).run()
        requests = prediction.build_model_run_requests(context)
        specs = ModelRunPlanner(context, artifacts, preflight_report).build(requests)
        write_model_run_plan(
            artifacts,
            runs,
            specs,
        )
        results = ModelRunExecutor(context, specs).run()
        prediction_summary = prediction.analyze(context, specs, results)
        if self.options.oracle_scores and not self.options.dry_run:
            from .validations.hicache.oracle_cost_replay.runner import run_suite

            prediction_summary["oracle_cost_replay"] = {
                config: run_suite(
                    artifacts.output_dir,
                    base_observations_path=base,
                    target_oracle_bundle_path=targets,
                    source_config_id=config,
                    jobs=self.options.model_run_jobs,
                    max_runs=self.options.max_predictions or None,
                )
                for config, base, targets in self.options.oracle_scores
            }
        prune_debug_details(
            (result.artifacts for result in results.values()),
            self.options.artifact_policy,
        )
        write_workflow_summary(
            context,
            specs=specs,
            preflight_report=preflight_report,
            results=results,
            prediction_summary=prediction_summary,
        )
        return 0

    def _selected_runs(self) -> list[ProfileRunRef]:
        if self.options.oracle_scores and (not self.options.evaluation or not self.options.artifact_policy.keep_debug_artifacts):
            raise SystemExit("Oracle-cost replay requires evaluation with full diagnostics.")
        selected_bases = self.options.source_config_ids or frozenset(dict(self.options.base_io_models))
        if self.options.oracle_scores and not {row[0] for row in self.options.oracle_scores} <= selected_bases:
            raise SystemExit("Oracle score bases must be selected evaluation sources.")
        if self.options.evaluation:
            if self.options.source_manifests or self.options.target_configs:
                raise SystemExit("Evaluation accepts --profile-run-dir, not prediction source/target inputs.")
            if self.options.base_io_models and self.options.hicache_io_model:
                raise SystemExit("Evaluation accepts either --hicache-io-model or --base-io-model, not both.")
            runs = ProfileRunDiscovery(self.options.profile_run_dirs, ()).discover()
        else:
            if self.options.profile_run_dirs:
                raise SystemExit("Prediction accepts --source-manifest, not --profile-run-dir.")
            if self.options.base_io_models:
                raise SystemExit("Prediction accepts --hicache-io-model, not --base-io-model.")
            if not self.options.source_manifests or not self.options.target_configs:
                raise SystemExit("Prediction requires --source-manifest and --target-config.")
            runs = ProfileRunDiscovery((), self.options.source_manifests).discover()
        runs = RunSelector(
            input_ids=self.options.input_ids,
            config_ids=self.options.config_ids,
        ).filter(runs)
        if not runs:
            raise SystemExit("No profile manifests matched the requested workflow.")
        return runs
