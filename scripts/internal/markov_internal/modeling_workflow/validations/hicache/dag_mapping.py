"""HiCache DAG 映射验证对象。"""

from __future__ import annotations

from typing import Any

from ...planning.specs import ModelRunRequest
from ...types import ModelRunResult, ModelOutputRequirement, ValidationSummary
from ..base_dag.preflight import DagTracePreflightCheck
from ..registry import RowValidation, count_blockers


class HiCacheDagMappingValidation(RowValidation):
    """验证 HiCache 事件到 Base DAG 的锚点覆盖和操作可见性。"""

    name = "hicache_dag_mapping"
    progress_detail = "HiCache DAG mapping"
    progress_unit = "run"

    def preflight_checks(self) -> tuple[type[DagTracePreflightCheck], ...]:
        """返回 full-DAG trace 通道检查。"""

        return (DagTracePreflightCheck,)

    def build_model_run_requests(self, context: Any) -> list[ModelRunRequest]:
        """请求 faithful replay DAG 诊断产物。"""

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
        """读取 HiCache DAG mapping 诊断产物并构造 row。"""

        artifacts = result.artifacts
        anchor = artifacts.load_if_present(artifacts.dag_anchor_coverage_json)
        visibility = artifacts.load_if_present(artifacts.dag_operation_visibility_json)
        visible_summary = visibility.get("summary") if isinstance(visibility.get("summary"), dict) else {}
        blockers = []
        if result.skipped:
            blockers.append(result.skip_reason or "skipped")
        if result.return_code != 0:
            blockers.append("model_command_failed")
        for path in (artifacts.dag_anchor_coverage_json, artifacts.dag_operation_visibility_json):
            if not path.is_file() and not result.skipped:
                blockers.append(f"missing_artifact:{path.name}")
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
        """返回 HiCache DAG mapping 的运行中指标。"""

        return {"anchor": f"{sum(1 for row in rows if row.get('anchor_coverage_ready'))}/{len(rows)}"}

    def build_summary(self, context: Any, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """汇总 HiCache DAG mapping 验证结果。"""

        run_count = len(rows)
        skipped_count = sum(1 for row in rows if row.get("skipped"))
        error_count = sum(1 for row in rows if row.get("return_code") not in (0, None))
        anchor_ready_count = sum(1 for row in rows if row.get("anchor_coverage_ready") is True)
        if run_count == 0:
            status = "EMPTY"
        elif error_count:
            status = "ERROR"
        elif anchor_ready_count == run_count:
            status = "OK"
        else:
            status = "CHECK"
        return {
            "schema": "trace_sim.modeling_workflow.validation.hicache_dag_mapping.v1",
            "validation": self.name,
            "status": status,
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
            "rows": rows,
        }

    def summary_text(self, summary: dict[str, Any]) -> str:
        """返回最终进度摘要。"""

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
        """转换成 workflow 统一 summary。"""

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
