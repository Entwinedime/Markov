"""final DAG 验证请求。"""

from __future__ import annotations

from typing import Any

from ..base_dag.diagnostics import DagDiagnosticsValidation
from ...types import ModelRunResult


class FinalDagValidation(DagDiagnosticsValidation):
    """验证最终 DAG；当前无有效 patch，因此 final DAG 等同 base DAG。"""

    name = "final_dag"
    schema = "trace_sim.modeling_workflow.validation.final_dag.v1"
    progress_detail = "final DAG diagnostics"

    def extend_row(self, row: dict[str, Any], result: ModelRunResult) -> None:
        """标记当前 final DAG 的实际来源。"""

        row["final_dag_source"] = "base_dag"
        row["patch_applied"] = False

    def build_summary(self, context: Any, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """汇总当前 final DAG 验证结果。"""

        summary = super().build_summary(context, rows)
        summary["final_dag_source"] = "base_dag"
        summary["patch_applied_count"] = sum(1 for row in rows if row.get("patch_applied") is True)
        return summary
