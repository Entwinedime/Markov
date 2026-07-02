"""HiCache final-state 验证对象。"""

from __future__ import annotations

from typing import Any

from ...types import ModelRunResult, ModelOutputRequirement, ValidationSummary
from ..registry import PredictionValidation, RowValidation, count_blockers
from .prediction_rows import build_prediction_row, summarize_final_state_rows
from .preflight.state_input_preflight import HiCacheStateInputPreflightCheck


class HiCacheFinalStateValidation(PredictionValidation, RowValidation):
    """验证 HiCache 预测 final state 是否匹配目标 oracle snapshot。"""

    name = "hicache_final_state"
    progress_detail = "HiCache final state"
    progress_unit = "prediction"
    cache_state_output_requirements = frozenset(
        {ModelOutputRequirement.MODULE_SUMMARY, ModelOutputRequirement.MODULE_VALIDATION}
    )

    def preflight_checks(self) -> tuple[type[HiCacheStateInputPreflightCheck], ...]:
        """返回 HiCache state/oracle 输入检查。"""

        return (HiCacheStateInputPreflightCheck,)

    def build_row(self, context: Any, result: ModelRunResult) -> dict[str, Any]:
        """读取 cache-state 验证产物并构造 prediction row。"""

        return build_prediction_row(result)

    def running_metrics(self, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """返回 final-state 验证的运行中指标。"""

        return {
            "ready": f"{sum(1 for row in rows if row.get('validation_ready'))}/{len(rows)}",
            "exact": f"{sum(1 for row in rows if row.get('final_state_match'))}/{len(rows)}",
        }

    def build_summary(self, context: Any, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """汇总 final-state 验证结果。"""

        prediction_count = len(rows)
        validation_ready_count = sum(1 for row in rows if row.get("validation_ready") is True)
        exact_count = sum(1 for row in rows if row.get("final_state_match") is True)
        skipped_count = sum(1 for row in rows if row.get("skipped"))
        error_count = sum(1 for row in rows if row.get("return_code") not in (0, None))
        if prediction_count == 0:
            status = "EMPTY"
        elif error_count:
            status = "ERROR"
        elif validation_ready_count != prediction_count:
            status = "NOT_READY"
        elif exact_count == prediction_count:
            status = "OK"
        else:
            status = "MISMATCH"
        by_scope: dict[str, Any] = {}
        if "self" in context.options.prediction_scope:
            by_scope["self"] = summarize_final_state_rows([row for row in rows if row.get("is_self")], scope="self")
        if "cross" in context.options.prediction_scope:
            by_scope["cross"] = summarize_final_state_rows(
                [row for row in rows if not row.get("is_self")],
                scope="cross",
            )
        return {
            "schema": "trace_sim.modeling_workflow.validation.hicache_final_state.v1",
            "validation": self.name,
            "status": status,
            "prediction_count": prediction_count,
            "validation_ready_count": validation_ready_count,
            "state_model_fact_ready_count": sum(1 for row in rows if row.get("state_model_fact_ready") is True),
            "final_state_match_count": exact_count,
            "skipped_count": skipped_count,
            "error_count": error_count,
            "blocker_counts": count_blockers(rows, "validation_errors"),
            "by_scope": by_scope,
            "rows": rows,
        }

    def summary_text(self, summary: dict[str, Any]) -> str:
        """返回最终进度摘要。"""

        return (
            f"{summary['prediction_count']} predictions | "
            f"exact {summary['final_state_match_count']}/{summary['prediction_count']} | "
            f"ready {summary['validation_ready_count']}/{summary['prediction_count']}"
        )

    def validation_summary(
        self,
        context: Any,
        rows: list[dict[str, Any]],
        summary: dict[str, Any],
    ) -> ValidationSummary:
        """转换成 workflow 统一 summary。"""

        return ValidationSummary(
            name=self.name,
            status=summary["status"],
            selected_run_count=len(rows),
            ready_count=int(summary["validation_ready_count"]),
            exact_count=int(summary["final_state_match_count"]),
            skipped_count=int(summary["skipped_count"]),
            blocker_counts=summary["blocker_counts"],
            artifact_paths={"summary": str(context.artifacts.validation_summary_path(self.name))},
            payload=summary,
        )
