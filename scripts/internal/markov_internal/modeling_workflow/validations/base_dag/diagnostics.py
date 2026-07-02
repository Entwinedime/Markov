"""DAG 诊断类验证的公共模板。"""

from __future__ import annotations

from typing import Any

from ...planning.specs import ModelRunRequest
from ...types import ModelRunResult, ModelOutputRequirement, ValidationSummary
from ..registry import RowValidation, count_blockers
from .preflight import DagTracePreflightCheck


class DagDiagnosticsValidation(RowValidation):
    """读取 faithful replay DAG 诊断产物的验证基类。"""

    schema: str
    progress_unit = "run"

    def preflight_checks(self) -> tuple[type[DagTracePreflightCheck], ...]:
        """返回 full-DAG trace 通道检查。"""

        return (DagTracePreflightCheck,)

    def build_model_run_requests(self, context: Any) -> list[ModelRunRequest]:
        """为每个 source profile 请求一次 faithful replay DAG 诊断。"""

        return [
            ModelRunRequest(
                mode="faithful_replay",
                source_profile=run,
                target_profile=None,
                output_requirements=frozenset({ModelOutputRequirement.BASE_DAG_DIAGNOSTICS}),
                validation_name=self.name,
            )
            for run in context.runs
        ]

    def build_row(self, context: Any, result: ModelRunResult) -> dict[str, Any]:
        """读取单次 DAG 诊断产物并构造验证 row。"""

        row = self._base_row(result)
        self.extend_row(row, result)
        return row

    def extend_row(self, row: dict[str, Any], result: ModelRunResult) -> None:
        """允许子类补充语义字段。"""

    def running_metrics(self, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """返回 DAG 诊断验证的运行中指标。"""

        return {"ready": f"{sum(1 for row in rows if row.get('ready'))}/{len(rows)}"}

    def build_summary(self, context: Any, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """汇总 DAG 诊断验证结果。"""

        run_count = len(rows)
        ready_count = sum(1 for row in rows if row.get("ready") is True)
        skipped_count = sum(1 for row in rows if row.get("skipped"))
        error_count = sum(1 for row in rows if row.get("return_code") not in (0, None))
        if run_count == 0:
            status = "EMPTY"
        elif error_count:
            status = "ERROR"
        elif ready_count == run_count:
            status = "OK"
        else:
            status = "CHECK"
        return {
            "schema": self.schema,
            "validation": self.name,
            "status": status,
            "run_count": run_count,
            "ready_count": ready_count,
            "skipped_count": skipped_count,
            "error_count": error_count,
            "blocker_counts": count_blockers(rows, "blockers"),
            "rows": rows,
        }

    def summary_text(self, summary: dict[str, Any]) -> str:
        """返回 DAG 诊断验证的最终摘要。"""

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
        """转换成 workflow 统一 summary。"""

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
        faithful = dag_quality.get("faithful_replay") if isinstance(dag_quality.get("faithful_replay"), dict) else {}
        blockers = []
        if result.skipped:
            blockers.append(result.skip_reason or "skipped")
        if result.return_code != 0:
            blockers.append("model_command_failed")
        for path in (artifacts.dag_quality_json, artifacts.dag_analysis_json):
            if not path.is_file() and not result.skipped:
                blockers.append(f"missing_artifact:{path.name}")
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
            "ready": not blockers and faithful.get("ready") is True,
            "faithful_replay_ready": faithful.get("ready"),
            "faithful_replay_relative_error": faithful.get("relative_error"),
            "blockers": blockers,
            "dag_quality_path": str(artifacts.dag_quality_json),
            "dag_analysis_path": str(artifacts.dag_analysis_json),
        }
