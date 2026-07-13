"""Shared row and summary logic for DAG diagnostic validations."""

from __future__ import annotations

from typing import Any

from ...progress import count_text
from ...types import ModelRunResult, ValidationSummary
from ..dag import DagArtifactValidation
from ..registry import count_blockers, readiness_status


class DagDiagnosticsValidation(DagArtifactValidation):
    """Base class that interprets faithful-replay DAG diagnostic artifacts."""

    schema: str

    def build_row(self, context: Any, result: ModelRunResult) -> dict[str, Any]:
        """Read one model run's DAG diagnostics and build its validation row."""

        row = self._base_row(result)
        self.extend_row(row, result)
        return row

    def extend_row(self, row: dict[str, Any], result: ModelRunResult) -> None:
        """Allow a specialization to add semantic fields to a base row."""

    def running_metrics(self, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """Return the running readiness metric for DAG diagnostics."""

        return {"ready": count_text(sum(1 for row in rows if row.get("ready")), len(rows))}

    def build_summary(self, context: Any, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """Aggregate readiness, skips, errors, and blockers across DAG rows."""

        run_count = len(rows)
        ready_count = sum(1 for row in rows if row.get("ready") is True)
        faithful_replay_ready_count = sum(1 for row in rows if row.get("faithful_replay_ready") is True)
        skipped_count = sum(1 for row in rows if row.get("skipped"))
        error_count = sum(1 for row in rows if row.get("return_code") not in (0, None))
        return {
            "schema": self.schema,
            "validation": self.name,
            "status": readiness_status(run_count, error_count, ready_count),
            "run_count": run_count,
            "ready_count": ready_count,
            "faithful_replay_ready_count": faithful_replay_ready_count,
            "skipped_count": skipped_count,
            "error_count": error_count,
            "blocker_counts": count_blockers(rows, "blockers"),
            "faithful_replay_diagnostic_blocker_counts": count_blockers(rows, "faithful_replay_blockers"),
        }

    def summary_text(self, summary: dict[str, Any]) -> str:
        """Render the final DAG-diagnostics progress summary."""

        return (
            f"{summary['run_count']} runs | "
            f"ready {summary['ready_count']}/{summary['run_count']} | "
            f"skipped {summary['skipped_count']}"
        )

    def validation_summary(
        self,
        context: Any,
        rows: list[dict[str, Any]],
        summary: dict[str, Any],
    ) -> ValidationSummary:
        """Convert DAG-specific counts to the workflow summary contract."""

        return ValidationSummary(
            name=self.name,
            status=summary["status"],
            selected_run_count=len(rows),
            ready_count=int(summary["ready_count"]),
            skipped_count=int(summary["skipped_count"]),
            blocker_counts=summary["blocker_counts"],
            artifact_paths={"summary": str(context.artifacts.validation_summary_path(self.name))},
            payload=summary,
        )

    @staticmethod
    def _base_row(result: ModelRunResult) -> dict[str, Any]:
        artifacts = result.artifacts
        dag_quality = artifacts.load_if_present(artifacts.dag_quality_json)
        dag_analysis = artifacts.load_if_present(artifacts.dag_analysis_json)
        dag_build = dag_quality.get("dag_build") if isinstance(dag_quality.get("dag_build"), dict) else {}
        faithful = dag_quality.get("faithful_replay") if isinstance(dag_quality.get("faithful_replay"), dict) else {}
        blockers = []
        if result.skipped:
            blockers.append(result.skip_reason or "skipped")
        if result.return_code != 0:
            blockers.append("model_command_failed")
        blockers.extend(
            f"missing_artifact:{path.name}"
            for path in (artifacts.dag_quality_json, artifacts.dag_analysis_json)
            if not path.is_file() and not result.skipped
        )
        node_count = dag_build.get("node_count")
        edge_count = dag_build.get("edge_count")
        if artifacts.dag_quality_json.is_file() and not dag_build:
            blockers.append("dag_build_summary_missing")
        elif not isinstance(node_count, int) or node_count <= 0:
            blockers.append("dag_graph_empty")
        elif not isinstance(edge_count, int) or edge_count <= 0:
            blockers.append("dag_edges_empty")
        analysis_blockers = dag_analysis.get("blockers") if isinstance(dag_analysis.get("blockers"), list) else []
        blockers.extend(f"dag_analysis:{blocker}" for blocker in analysis_blockers)
        faithful_blockers = faithful.get("blockers") if isinstance(faithful.get("blockers"), list) else []
        faithful_diagnostic_blockers = [str(blocker) for blocker in faithful_blockers]
        if faithful and faithful.get("ready") is not True and not faithful_diagnostic_blockers:
            faithful_diagnostic_blockers.append("faithful_replay_not_ready")
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
            "ready": not blockers,
            "dag_node_count": node_count,
            "dag_edge_count": edge_count,
            "dag_analysis_blockers": [str(blocker) for blocker in analysis_blockers],
            "faithful_replay_ready": faithful.get("ready"),
            "faithful_replay_relative_error": faithful.get("relative_error"),
            "faithful_replay_blockers": faithful_diagnostic_blockers,
            "blockers": blockers,
            "dag_quality_path": str(artifacts.dag_quality_json),
            "dag_analysis_path": str(artifacts.dag_analysis_json),
        }
