"""建模验证对象的接口、公共模板和注册表。"""

from __future__ import annotations

from abc import ABC, abstractmethod
from typing import TYPE_CHECKING, Any

from ...common.io import write_json
from ..planning.profile_runs import PredictionMatrixBuilder
from ..planning.specs import ModelRunRequest
from ..types import ModelOutputRequirement, ModelRunResult, ModelRunSpec, ValidationSummary

if TYPE_CHECKING:
    from ..context import WorkflowContext
    from ..preflight import PreflightCheck


class ValidationRequest(ABC):
    """Python 侧验证对象接口。"""

    name: str

    @abstractmethod
    def preflight_checks(self) -> tuple[type["PreflightCheck"], ...]:
        """返回该验证依赖的前置检查。"""

    @abstractmethod
    def build_model_run_requests(self, context: "WorkflowContext") -> list["ModelRunRequest"]:
        """返回该验证需要的 C++ 运行请求。"""

    @abstractmethod
    def analyze(
        self,
        context: "WorkflowContext",
        specs: list[ModelRunSpec],
        results: dict[str, ModelRunResult],
    ) -> ValidationSummary:
        """读取 C++ 产物并写出验证结果。"""

    def selected_specs(self, specs: list[ModelRunSpec]) -> list[ModelRunSpec]:
        """筛选属于当前验证对象的模型运行。"""

        return [spec for spec in specs if self.name in spec.validation_requests]


class RowValidation(ValidationRequest):
    """以逐运行 row 为核心的验证模板。"""

    progress_detail: str
    progress_unit = "run"

    def analyze(
        self,
        context: "WorkflowContext",
        specs: list[ModelRunSpec],
        results: dict[str, ModelRunResult],
    ) -> ValidationSummary:
        """执行通用 row -> summary 验证流程。"""

        rows: list[dict[str, Any]] = []
        selected = self.selected_specs(specs)
        progress = context.reporter.start_stage(
            self.name,
            len(selected),
            self.progress_detail,
            unit=self.progress_unit,
        )
        for spec in selected:
            row = self.build_row(context, results[spec.run_id])
            rows.append(row)
            write_json(context.artifacts.validation_row_path(self.name, spec.run_id), row)
            progress.advance(self.running_metrics(rows))

        summary = self.build_summary(context, rows)
        write_json(context.artifacts.validation_summary_path(self.name), summary)
        progress.finish(str(summary["status"]), self.summary_text(summary))
        return self.validation_summary(context, rows, summary)

    @abstractmethod
    def build_row(self, context: "WorkflowContext", result: ModelRunResult) -> dict[str, Any]:
        """从单个模型运行结果构造验证 row。"""

    @abstractmethod
    def running_metrics(self, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """返回动态进度条上的精简指标。"""

    @abstractmethod
    def build_summary(self, context: "WorkflowContext", rows: list[dict[str, Any]]) -> dict[str, Any]:
        """从验证 row 构造 summary payload。"""

    @abstractmethod
    def summary_text(self, summary: dict[str, Any]) -> str:
        """返回最终进度行上的摘要文本。"""

    @abstractmethod
    def validation_summary(
        self,
        context: "WorkflowContext",
        rows: list[dict[str, Any]],
        summary: dict[str, Any],
    ) -> ValidationSummary:
        """转换成 workflow 统一 summary 数据结构。"""


class PredictionValidation(ValidationRequest):
    """基于同一 workload prediction matrix 的 cache-state 验证模板。"""

    cache_state_output_requirements: frozenset[ModelOutputRequirement]

    def build_model_run_requests(self, context: "WorkflowContext") -> list[ModelRunRequest]:
        """为选中 prediction matrix 构造 cache-state 模型运行请求。"""

        predictions = PredictionMatrixBuilder(
            runs=context.runs,
            source_config_ids=context.options.source_config_ids,
            target_config_ids=context.options.target_config_ids,
            prediction_scope=context.options.prediction_scope,
            max_predictions=context.options.max_predictions,
        ).build()
        return [
            ModelRunRequest(
                mode="cache_state",
                source_profile=prediction.source,
                target_profile=prediction.target,
                output_requirements=self.cache_state_output_requirements,
                validation_name=self.name,
                prediction=prediction,
            )
            for prediction in predictions
        ]


def validation_names() -> tuple[str, ...]:
    """返回 CLI 支持的验证对象名称。"""

    return (
        "base_dag",
        "final_dag",
        "hicache_dag_mapping",
        "hicache_final_state",
        "hicache_transition",
    )


def validation_by_name(name: str) -> ValidationRequest:
    """按稳定 CLI 名称构造验证对象。"""

    if name == "base_dag":
        from .base_dag.request import BaseDagValidation

        return BaseDagValidation()
    if name == "final_dag":
        from .final_dag.request import FinalDagValidation

        return FinalDagValidation()
    if name == "hicache_dag_mapping":
        from .hicache.dag_mapping import HiCacheDagMappingValidation

        return HiCacheDagMappingValidation()
    if name == "hicache_final_state":
        from .hicache.final_state import HiCacheFinalStateValidation

        return HiCacheFinalStateValidation()
    if name == "hicache_transition":
        from .hicache.transition.request import HiCacheTransitionValidation

        return HiCacheTransitionValidation()
    raise KeyError(name)


def count_blockers(rows: list[dict[str, Any]], key: str) -> dict[str, int]:
    """统计 row 中的阻塞原因。"""

    counts: dict[str, int] = {}
    for row in rows:
        values = row.get(key)
        if isinstance(values, list):
            for value in values:
                text = str(value)
                counts[text] = counts.get(text, 0) + 1
        elif values:
            text = str(values)
            counts[text] = counts.get(text, 0) + 1
    return dict(sorted(counts.items()))
