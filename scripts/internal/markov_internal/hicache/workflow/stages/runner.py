"""HiCache validation workflow 的面向对象 stage runner。"""

from __future__ import annotations

from abc import ABC, abstractmethod
from pathlib import Path
from typing import Any

from ....common.io import write_json
from ...matrix.reports.quality import build_quality_report
from ..config.modeling import write_runner_configs_for_targets
from ..core.context import WorkflowRunContext
from ..output.progress import (
    final_state_rows_done_summary,
    final_state_running_metrics,
    quality_done_summary,
    quality_running_metrics,
    transition_done_summary,
    transition_running_metrics,
)
from .final_state import (
    prediction_specs_for_options,
    run_final_state_predictions,
    summarize_existing_predictions,
)
from .transition import run_transition_stage


class WorkflowStageRunner(ABC):
    """单个 workflow stage 生命周期的抽象基类。"""

    name: str

    @abstractmethod
    def run(self, context: WorkflowRunContext, summaries: dict[str, Any]) -> Any:
        """执行阶段，并把阶段 summary 写回 workflow summaries。"""


class QualityStageRunner(WorkflowStageRunner):
    """执行 HiCache workflow input quality 阶段。"""

    name = "quality"

    def run(self, context: WorkflowRunContext, summaries: dict[str, Any]) -> dict[str, Any]:
        """运行 quality audit，并通过统一 progress reporter 汇报阶段进度。"""

        rows: list[dict[str, Any]] = []
        visible = self.name in context.stages
        progress = None
        if visible:
            detail = (
                f"inputs {len({run.input_id for run in context.runs})} | "
                f"configs {len({run.config_id for run in context.runs})}"
            )
            progress = context.reporter.start_stage(self.name, len(context.runs), detail, unit="run")

        def on_row(row: dict[str, Any]) -> None:
            """接收单个 run 的 quality audit 结果并推进进度。"""

            rows.append(row)
            if progress is not None:
                progress.advance(quality_running_metrics(rows))

        report = build_quality_report(
            context.runs,
            context.output_dir,
            audit_dir=context.artifacts.quality_audit_dir,
            summary_path=context.artifacts.quality_summary_path,
            on_row=on_row,
            require_validation_evidence=bool({"final-state", "transition"} & context.stages),
            validate_diagnostic_coverage="transition" in context.stages,
            require_cross_config_contract=(
                bool({"final-state", "transition"} & context.stages) and "cross" in context.prediction_scope
            ),
            show_workload_sequence=context.show_workload_sequence,
        )
        if progress is not None:
            status, summary = quality_done_summary(report)
            progress.finish(status, summary)
        summaries["quality"] = report
        return report


class FinalStateStageRunner(WorkflowStageRunner):
    """执行 final-state prediction，并写出 self/cross summary。"""

    name = "final-state"

    def run(self, context: WorkflowRunContext, summaries: dict[str, Any]) -> list[dict[str, Any]]:
        """运行 final-state 阶段，并把 prediction row 汇总到阶段 summary。"""

        quality_report = summaries.get("quality") if isinstance(summaries.get("quality"), dict) else {}
        specs = prediction_specs_for_options(context.runs, context.final_state_options)
        rows: list[dict[str, Any]] = []
        detail = self._detail(specs, dry_run=context.final_state_options.dry_run)
        progress = context.reporter.start_stage(self.name, len(specs), detail, unit="prediction")
        runner_configs = write_runner_configs_for_targets(
            [spec.target for spec in specs],
            context.artifacts.runner_config_dir,
        )

        def on_row(row: dict[str, Any]) -> None:
            """接收单个 prediction row 并推进进度。"""

            rows.append(row)
            progress.advance(final_state_running_metrics(rows))

        final_rows = run_final_state_predictions(
            context.runs,
            context.output_dir,
            context.final_state_options,
            quality_report,
            runner_configs=runner_configs,
            on_row=on_row,
        )
        status, summary = final_state_rows_done_summary(final_rows)
        progress.finish(status, summary)
        self._write_scope_summaries(context, summaries)
        return final_rows

    def _write_scope_summaries(
        self,
        context: WorkflowRunContext,
        summaries: dict[str, Any],
    ) -> None:
        """按 prediction scope 写出 self/cross final-state summary。"""

        if "self" in context.prediction_scope:
            self_summary = summarize_existing_predictions(
                context.runs,
                context.output_dir,
                context.final_state_options,
                scope={"self"},
                schema="trace_sim.hicache.state_workflow.final_state_self.v1",
                stage="final-state:self",
            )
            write_json(context.artifacts.final_state_self_summary_path, self_summary)
            summaries["final_state_self"] = self_summary
        if "cross" in context.prediction_scope:
            cross_summary = summarize_existing_predictions(
                context.runs,
                context.output_dir,
                context.final_state_options,
                scope={"cross"},
                schema="trace_sim.hicache.state_workflow.final_state_cross.v1",
                stage="final-state:cross",
            )
            write_json(context.artifacts.final_state_cross_summary_path, cross_summary)
            summaries["final_state_cross"] = cross_summary

    @staticmethod
    def _detail(specs: list[Any], *, dry_run: bool) -> str:
        """生成 final-state 阶段开始时展示的简短范围说明。"""

        self_count = sum(1 for spec in specs if spec.is_self)
        cross_count = len(specs) - self_count
        parts = [f"self {self_count}", f"cross {cross_count}"]
        if dry_run:
            parts.append("dry-run")
        return " | ".join(parts)


class TransitionStageRunner(WorkflowStageRunner):
    """基于已有 final-state prediction 执行 transition exactness。"""

    name = "transition"

    def run(self, context: WorkflowRunContext, summaries: dict[str, Any]) -> dict[str, Any]:
        """运行 transition 阶段，并把 exactness summary 写回 workflow。"""

        rows: list[dict[str, Any]] = []
        total = estimate_transition_count(context.output_dir)
        progress = context.reporter.start_stage(self.name, total, self._detail(context), unit="prediction")

        def on_row(row: dict[str, Any]) -> None:
            """接收单个 transition comparison row 并推进进度。"""

            rows.append(row)
            progress.advance(transition_running_metrics(rows))

        summary = run_transition_stage(
            context.output_dir,
            context.transition_options,
            summary_path=context.artifacts.transition_summary_path,
            catalog_dir=context.artifacts.transition_catalog_dir,
            on_row=on_row,
        )
        status, summary_text = transition_done_summary(summary)
        progress.finish(status, summary_text)
        summaries["transition"] = summary
        return summary

    @staticmethod
    def _detail(context: WorkflowRunContext) -> str:
        """生成 transition 阶段开始时展示的可选产物说明。"""

        parts: list[str] = []
        if context.transition_options.emit_catalog:
            parts.append("catalog")
        if context.transition_options.emit_gates:
            parts.append("gates")
        if context.transition_options.dry_run:
            parts.append("dry-run")
        return " | ".join(parts)


def estimate_transition_count(output_dir: Path) -> int:
    """从已有 prediction matrix row 估算 transition 行数。"""

    return len(list((output_dir / "predictions").glob("*/*/matrix_row.json")))
