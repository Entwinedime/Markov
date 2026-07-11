"""Validation request for the current final DAG."""

from __future__ import annotations

from typing import Any

from ..base_dag.diagnostics import DagDiagnosticsValidation
from ...types import ModelRunResult


class FinalDagValidation(DagDiagnosticsValidation):
    """Validate the final DAG, which remains the base DAG in Phase 0/1."""

    name = "final_dag"
    schema = "trace_sim.modeling_workflow.validation.final_dag.v1"
    progress_detail = "final DAG diagnostics"

    def extend_row(self, row: dict[str, Any], result: ModelRunResult) -> None:
        """Record that the empty Phase 0/1 patch leaves the base DAG active."""

        row["final_dag_source"] = "base_dag"
        row["patch_applied"] = False

    def build_summary(self, context: Any, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """Add final-DAG provenance to the common diagnostic summary."""

        summary = super().build_summary(context, rows)
        summary["final_dag_source"] = "base_dag"
        summary["patch_applied_count"] = sum(1 for row in rows if row.get("patch_applied") is True)
        return summary
