"""final DAG 验证请求。"""

from __future__ import annotations

from typing import Any

from ....common.io import load_json, write_json
from ...planning.profile_runs import PredictionMatrixBuilder
from ...planning.specs import ModelRunRequest
from ...types import ModelOutputRequirement, ModelRunResult, ModelRunSpec, ValidationSummary
from ..base_dag.preflight import DagTracePreflightCheck
from ..hicache.preflight.state_input_preflight import HiCacheStateInputPreflightCheck
from ..registry import RowValidation, count_blockers


class FinalDagValidation(RowValidation):
    """运行 cache_patch prediction matrix，并按 target E2E 评估 final DAG。"""

    name = "final_dag"
    schema = "trace_sim.modeling_workflow.validation.final_dag.v1"
    progress_detail = "final DAG cache patch baseline"
    progress_unit = "prediction"
    e2e_relative_error_threshold = 0.10

    def preflight_checks(self) -> tuple[type[DagTracePreflightCheck] | type[HiCacheStateInputPreflightCheck], ...]:
        """返回 final DAG 需要的 full-DAG trace 和 HiCache state 输入检查。"""

        return (DagTracePreflightCheck, HiCacheStateInputPreflightCheck)

    def build_model_run_requests(self, context: Any) -> list[ModelRunRequest]:
        """为选中 prediction matrix 请求 cache_patch 运行。"""

        predictions = PredictionMatrixBuilder(
            runs=context.runs,
            source_config_ids=context.options.source_config_ids,
            target_config_ids=context.options.target_config_ids,
            prediction_scope=context.options.prediction_scope,
            max_predictions=context.options.max_predictions,
        ).build()
        return [
            ModelRunRequest(
                mode="cache_patch",
                source_profile=prediction.source,
                target_profile=prediction.target,
                output_requirements=frozenset({ModelOutputRequirement.MODULE_SUMMARY}),
                validation_name=self.name,
                prediction=prediction,
            )
            for prediction in predictions
        ]

    def build_row(self, context: Any, result: ModelRunResult) -> dict[str, Any]:
        """读取 cache_patch 产物并构造 final DAG E2E row。"""

        return self._build_row_with_target_actual(result, None)

    def analyze(
        self,
        context: Any,
        specs: list[ModelRunSpec],
        results: dict[str, ModelRunResult],
    ) -> ValidationSummary:
        """读取所有 cache_patch run 后，用 target trace-real E2E 构造 final DAG rows。"""

        rows: list[dict[str, Any]] = []
        selected = self.selected_specs(specs)
        target_actuals = collect_trace_real_e2e_by_source_run(selected, results)
        progress = context.reporter.start_stage(
            self.name,
            len(selected),
            self.progress_detail,
            unit=self.progress_unit,
        )
        for spec in selected:
            result = results[spec.run_id]
            target_actual = None
            if spec.prediction is not None:
                target_actual = target_actuals.get(spec.prediction.target.run_id)
            row = self._build_row_with_target_actual(result, target_actual)
            rows.append(row)
            write_json(context.artifacts.validation_row_path(self.name, spec.run_id), row)
            progress.advance(self.running_metrics(rows))

        summary = self.build_summary(context, rows)
        write_json(context.artifacts.validation_summary_path(self.name), summary)
        progress.finish(str(summary["status"]), self.summary_text(summary))
        return self.validation_summary(context, rows, summary)

    def _build_row_with_target_actual(
        self, result: ModelRunResult, target_trace_actual_e2e_ns: int | None
    ) -> dict[str, Any]:
        """用指定 target trace-real E2E 构造 final DAG row。"""

        spec = result.spec
        if spec.prediction is None:
            raise ValueError(f"missing prediction metadata for final DAG model run: {spec.run_id}")
        prediction = spec.prediction
        artifacts = result.artifacts
        prediction_json = artifacts.load_if_present(artifacts.prediction_json)
        run_summary = artifacts.load_if_present(artifacts.run_summary_json)
        hicache_summary = load_hicache_summary(artifacts.model_summary_json)
        predicted_e2e_ns = optional_int(prediction_json.get("predicted_e2e_ns"))
        source_real_e2e_ns = optional_int(run_summary.get("real_e2e_ns"))
        target_actual_e2e_ns = target_trace_actual_e2e_ns
        relative_error = (
            abs(predicted_e2e_ns - target_actual_e2e_ns) / target_actual_e2e_ns
            if predicted_e2e_ns is not None and target_actual_e2e_ns
            else None
        )
        dag_mutations = optional_int(hicache_summary.get("dag_mutations")) or 0
        blockers = row_blockers(result, predicted_e2e_ns, target_actual_e2e_ns, artifacts.model_summary_json.is_file())
        ready = not blockers and relative_error is not None
        return {
            "model_run_id": spec.run_id,
            "label": prediction.label,
            "input_id": prediction.input_id,
            "source_config_id": prediction.source.config_id,
            "target_config_id": prediction.target.config_id,
            "source_run_id": prediction.source.run_id,
            "target_run_id": prediction.target.run_id,
            "is_self": prediction.is_self,
            "output_dir": str(spec.output_dir),
            "return_code": result.return_code,
            "elapsed_sec": result.elapsed_sec,
            "skipped": result.skipped,
            "dry_run": result.dry_run,
            "skip_reason": result.skip_reason or None,
            "ready": ready,
            "within_10pct": ready
            and relative_error is not None
            and relative_error <= self.e2e_relative_error_threshold,
            "predicted_e2e_ns": predicted_e2e_ns,
            "target_actual_e2e_ns": target_actual_e2e_ns,
            "target_actual_source": "target_trace_real_e2e_ns" if target_actual_e2e_ns is not None else None,
            "source_real_e2e_ns": source_real_e2e_ns,
            "absolute_error_ns": predicted_e2e_ns - target_actual_e2e_ns
            if predicted_e2e_ns is not None and target_actual_e2e_ns is not None
            else None,
            "relative_error": relative_error,
            "threshold": self.e2e_relative_error_threshold,
            "final_dag_source": "cache_patch",
            "patch_config_enabled": bool(
                hicache_summary.get("target_config", {}).get("enable_dag_patch")
                if isinstance(hicache_summary.get("target_config"), dict)
                else False
            ),
            "patch_applied": dag_mutations > 0,
            "dag_mutations": dag_mutations,
            "blockers": blockers,
            "prediction_path": str(artifacts.prediction_json),
            "run_summary_path": str(artifacts.run_summary_json),
            "model_summary_path": str(artifacts.model_summary_json),
            "log_path": str(result.artifacts.model_log),
            "execution_error_tail": result.execution_error_tail if result.return_code != 0 else "",
        }

    def running_metrics(self, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """返回 final DAG 验证的运行中指标。"""

        return {
            "ready": f"{sum(1 for row in rows if row.get('ready'))}/{len(rows)}",
            "under10": f"{sum(1 for row in rows if row.get('within_10pct'))}/{len(rows)}",
        }

    def build_summary(self, context: Any, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """汇总 final DAG E2E 验证结果。"""

        prediction_count = len(rows)
        ready_count = sum(1 for row in rows if row.get("ready") is True)
        within_threshold_count = sum(1 for row in rows if row.get("within_10pct") is True)
        skipped_count = sum(1 for row in rows if row.get("skipped"))
        error_count = sum(1 for row in rows if row.get("return_code") not in (0, None))
        if prediction_count == 0:
            status = "EMPTY"
        elif error_count:
            status = "ERROR"
        elif ready_count != prediction_count:
            status = "NOT_READY"
        elif within_threshold_count == prediction_count:
            status = "OK"
        else:
            status = "MISMATCH"
        rel_errors = [
            float(row["relative_error"]) for row in rows if isinstance(row.get("relative_error"), (int, float))
        ]
        return {
            "schema": self.schema,
            "validation": self.name,
            "status": status,
            "prediction_count": prediction_count,
            "ready_count": ready_count,
            "within_10pct_count": within_threshold_count,
            "skipped_count": skipped_count,
            "error_count": error_count,
            "patch_applied_count": sum(1 for row in rows if row.get("patch_applied") is True),
            "dag_mutation_count": sum(int(row.get("dag_mutations") or 0) for row in rows),
            "threshold": self.e2e_relative_error_threshold,
            "max_relative_error": max(rel_errors) if rel_errors else None,
            "mean_relative_error": sum(rel_errors) / len(rel_errors) if rel_errors else None,
            "by_scope": {
                "self": summarize_e2e_rows([row for row in rows if row.get("is_self")]),
                "cross": summarize_e2e_rows([row for row in rows if not row.get("is_self")]),
            },
            "blocker_counts": count_blockers(rows, "blockers"),
            "rows": rows,
        }

    def summary_text(self, summary: dict[str, Any]) -> str:
        """返回 final DAG 验证的最终摘要。"""

        return (
            f"{summary['prediction_count']} predictions | "
            f"under10 {summary['within_10pct_count']}/{summary['prediction_count']} | "
            f"ready {summary['ready_count']}/{summary['prediction_count']} | "
            f"patched {summary['patch_applied_count']}"
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
            exact_count=int(summary["within_10pct_count"]),
            skipped_count=int(summary["skipped_count"]),
            blocker_counts=summary["blocker_counts"],
            artifact_paths={"summary": str(context.artifacts.validation_summary_path(self.name))},
            payload=summary,
        )


def load_hicache_summary(path: Any) -> dict[str, Any]:
    """从 model_summary.json 中提取 HiCache summary。"""

    if not path.is_file():
        return {}
    payload = load_json(path)
    modules = payload.get("modules") if isinstance(payload, dict) else []
    if not isinstance(modules, list):
        return {}
    for module in modules:
        if isinstance(module, dict) and module.get("name") == "HiCacheModule":
            hicache = module.get("hicache")
            return hicache if isinstance(hicache, dict) else {}
    return {}


def row_blockers(
    result: ModelRunResult,
    predicted_e2e_ns: int | None,
    target_actual_e2e_ns: int | None,
    has_model_summary: bool,
) -> list[str]:
    """构造 final DAG row blocker 列表。"""

    blockers: list[str] = []
    if result.skipped:
        blockers.append(result.skip_reason or "skipped")
    if result.return_code != 0:
        blockers.append("model_command_failed")
    if predicted_e2e_ns is None and not result.skipped:
        blockers.append("missing_prediction_json")
    if target_actual_e2e_ns is None and not result.skipped:
        blockers.append("missing_target_trace_real_e2e")
    if not has_model_summary and not result.skipped:
        blockers.append("missing_artifact:model_summary.json")
    return blockers


def optional_int(value: Any) -> int | None:
    """宽松解析整数。"""

    try:
        if value is None:
            return None
        return int(float(value))
    except (TypeError, ValueError):
        return None


def collect_trace_real_e2e_by_source_run(
    specs: list[ModelRunSpec],
    results: dict[str, ModelRunResult],
) -> dict[str, int]:
    """从本次 final DAG cache_patch runs 收集每个 source run 的 trace-real E2E。"""

    actuals: dict[str, int] = {}
    for spec in specs:
        result = results.get(spec.run_id)
        if result is None or result.skipped or result.return_code != 0:
            continue
        run_summary = result.artifacts.load_if_present(result.artifacts.run_summary_json)
        actual = optional_int(run_summary.get("real_e2e_ns"))
        if actual is not None and actual > 0:
            actuals[spec.source_profile.run_id] = actual
    return actuals


def summarize_e2e_rows(rows: list[dict[str, Any]]) -> dict[str, Any]:
    """汇总一组 final DAG E2E rows。"""

    rel_errors = [float(row["relative_error"]) for row in rows if isinstance(row.get("relative_error"), (int, float))]
    return {
        "prediction_count": len(rows),
        "ready_count": sum(1 for row in rows if row.get("ready") is True),
        "within_10pct_count": sum(1 for row in rows if row.get("within_10pct") is True),
        "patch_applied_count": sum(1 for row in rows if row.get("patch_applied") is True),
        "max_relative_error": max(rel_errors) if rel_errors else None,
        "mean_relative_error": sum(rel_errors) / len(rel_errors) if rel_errors else None,
    }
