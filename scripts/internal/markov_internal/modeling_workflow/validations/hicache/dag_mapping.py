"""Validation of HiCache operation visibility and DAG-anchor coverage."""

from __future__ import annotations

from typing import Any

from ...progress import count_text
from ...types import ModelRunResult, ValidationSummary
from ..dag import DagArtifactValidation
from ..registry import count_blockers, readiness_status


class HiCacheDagMappingValidation(DagArtifactValidation):
    """Validate HiCache operation visibility and anchors in the base DAG."""

    name = "hicache_dag_mapping"
    progress_detail = "HiCache DAG mapping"

    def build_row(self, context: Any, result: ModelRunResult) -> dict[str, Any]:
        """Read mapping diagnostics and construct one per-profile row."""

        artifacts = result.artifacts
        anchor = artifacts.load_if_present(artifacts.dag_anchor_coverage_json)
        visibility = artifacts.load_if_present(artifacts.dag_operation_visibility_json)
        visible_summary = visibility.get("summary") if isinstance(visibility.get("summary"), dict) else {}
        blockers = []
        if result.skipped:
            blockers.append(result.skip_reason or "skipped")
        if result.return_code != 0:
            blockers.append("model_command_failed")
        blockers.extend(
            f"missing_artifact:{path.name}"
            for path in (artifacts.dag_anchor_coverage_json, artifacts.dag_operation_visibility_json)
            if not path.is_file() and not result.skipped
        )
        anchor_blockers = anchor.get("blockers") if isinstance(anchor.get("blockers"), list) else []
        return {
            "model_run_id": result.spec.run_id,
            "label": result.spec.label,
            "run_id": result.spec.source_profile.run_id,
            "config_id": result.spec.source_profile.config_id,
            "input_id": result.spec.source_profile.input_id,
            "output_dir": str(result.spec.output_dir),
            "return_code": result.return_code,
            "skipped": result.skipped,
            "skip_reason": result.skip_reason or None,
            "anchor_coverage_ready": not blockers and anchor.get("ready") is True,
            "anchor_blockers": [*anchor_blockers, *blockers],
            "visible_operation_count": visible_summary.get("visible_count"),
            "partially_visible_operation_count": visible_summary.get("partially_visible_count"),
            "invisible_operation_count": visible_summary.get("invisible_count"),
            "dag_anchor_coverage_path": str(artifacts.dag_anchor_coverage_json),
            "dag_operation_visibility_path": str(artifacts.dag_operation_visibility_json),
        }

    def running_metrics(self, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """Return the running anchor-readiness metric."""

        return {
            "anchor": count_text(
                sum(1 for row in rows if row.get("anchor_coverage_ready")),
                len(rows),
            )
        }

    def build_summary(self, context: Any, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """Aggregate anchor readiness and operation-visibility counts."""

        run_count = len(rows)
        skipped_count = sum(1 for row in rows if row.get("skipped"))
        error_count = sum(1 for row in rows if row.get("return_code") not in (0, None))
        anchor_ready_count = sum(1 for row in rows if row.get("anchor_coverage_ready") is True)
        return {
            "schema": "trace_sim.modeling_workflow.validation.hicache_dag_mapping.v1",
            "validation": self.name,
            "status": readiness_status(run_count, error_count, anchor_ready_count),
            "run_count": run_count,
            "anchor_ready_count": anchor_ready_count,
            "skipped_count": skipped_count,
            "error_count": error_count,
            "visible_operation_count": sum(int(row.get("visible_operation_count") or 0) for row in rows),
            "partially_visible_operation_count": sum(
                int(row.get("partially_visible_operation_count") or 0) for row in rows
            ),
            "invisible_operation_count": sum(int(row.get("invisible_operation_count") or 0) for row in rows),
            "blocker_counts": count_blockers(rows, "anchor_blockers"),
        }

    def summary_text(self, summary: dict[str, Any]) -> str:
        """Render the final mapping-validation progress summary."""

        return (
            f"{summary['run_count']} runs | "
            f"anchor {summary['anchor_ready_count']}/{summary['run_count']} | "
            f"visible-ops {summary['visible_operation_count']}"
        )

    def validation_summary(
        self,
        context: Any,
        rows: list[dict[str, Any]],
        summary: dict[str, Any],
    ) -> ValidationSummary:
        """Convert mapping results to the workflow summary contract."""

        return ValidationSummary(
            name=self.name,
            status=summary["status"],
            selected_run_count=len(rows),
            ready_count=int(summary["anchor_ready_count"]),
            skipped_count=int(summary["skipped_count"]),
            blocker_counts=summary["blocker_counts"],
            artifact_paths={"summary": str(context.artifacts.validation_summary_path(self.name))},
            payload=summary,
        )
