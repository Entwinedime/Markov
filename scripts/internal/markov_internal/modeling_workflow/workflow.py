"""Top-level orchestration for the host-side modeling workflow."""

from __future__ import annotations

from dataclasses import dataclass

from .artifacts import WorkflowArtifactLayout, prune_debug_details
from .context import WorkflowContext, WorkflowOptions
from .execution.model_executor import ModelRunExecutor
from .planning.plan_io import write_model_run_plan
from .planning.profile_runs import ProfileRunDiscovery
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
        prediction = HiCachePredictionRequest()
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
        return 0 if self.options.dry_run or prediction_summary["status"] == "READY" else 2

    def _selected_runs(self) -> list[ProfileRunRef]:
        if not self.options.source_manifests or not self.options.target_configs:
            raise SystemExit("Prediction requires --source-manifest and --target-config.")
        runs = ProfileRunDiscovery((), self.options.source_manifests).discover()
        if not runs:
            raise SystemExit("No profile manifests matched the requested workflow.")
        return runs
