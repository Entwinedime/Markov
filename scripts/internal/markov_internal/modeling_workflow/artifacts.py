"""统一 modeling workflow 的 artifact 路径布局。"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..common.io import load_json
from .types import ModelRunSpec


@dataclass(frozen=True)
class WorkflowArtifactLayout:
    """集中维护 workflow 级 artifact 路径。"""

    output_dir: Path

    @property
    def artifacts_dir(self) -> Path:
        return self.output_dir / "artifacts"

    @property
    def preflight_dir(self) -> Path:
        return self.artifacts_dir / "preflight"

    @property
    def model_runs_dir(self) -> Path:
        return self.output_dir / "model_runs"

    @property
    def validations_dir(self) -> Path:
        return self.artifacts_dir / "validations"

    @property
    def plan_path(self) -> Path:
        return self.artifacts_dir / "model_run_plan.json"

    @property
    def preflight_summary_path(self) -> Path:
        return self.output_dir / "preflight_summary.json"

    @property
    def model_runs_summary_path(self) -> Path:
        return self.artifacts_dir / "model_runs_summary.json"

    @property
    def workflow_summary_path(self) -> Path:
        return self.output_dir / "workflow_summary.json"

    def validation_summary_path(self, validation_name: str) -> Path:
        return self.validations_dir / validation_name / "summary.json"

    def validation_row_path(self, validation_name: str, model_run_id: str) -> Path:
        return self.validations_dir / validation_name / f"{model_run_id}.json"

    def model_run_dir(self, model_run_id: str) -> Path:
        return self.model_runs_dir / model_run_id

    def runner_config_path(self, model_run_id: str) -> Path:
        return self.model_run_dir(model_run_id) / "runner_config.json"

    def ensure_base_dirs(self) -> None:
        """创建 workflow 顶层稳定目录。"""

        for path in (
            self.artifacts_dir,
            self.preflight_dir,
            self.model_runs_dir,
            self.validations_dir,
        ):
            path.mkdir(parents=True, exist_ok=True)


@dataclass(frozen=True)
class ModelRunArtifacts:
    """一次 C++ modeling 执行的 artifact 路径索引。"""

    output_dir: Path

    @property
    def command_json(self) -> Path:
        return self.output_dir / "command.json"

    @property
    def model_log(self) -> Path:
        return self.output_dir / "model.log"

    @property
    def prediction_json(self) -> Path:
        return self.output_dir / "prediction.json"

    @property
    def run_summary_json(self) -> Path:
        return self.output_dir / "run_summary.json"

    @property
    def validation_json(self) -> Path:
        return self.output_dir / "validation.json"

    @property
    def model_summary_json(self) -> Path:
        return self.output_dir / "model_summary.json"

    @property
    def dag_quality_json(self) -> Path:
        return self.output_dir / "dag_quality.json"

    @property
    def dag_analysis_json(self) -> Path:
        return self.output_dir / "dag_analysis.json"

    @property
    def dag_anchor_coverage_json(self) -> Path:
        return self.output_dir / "dag_anchor_coverage.json"

    @property
    def dag_operation_visibility_json(self) -> Path:
        return self.output_dir / "dag_operation_visibility.json"

    @property
    def predicted_state_trace_json(self) -> Path:
        return self.output_dir / "predicted_target_cache_state_trace.json"

    def load_if_present(self, path: Path) -> dict[str, Any]:
        """读取可选 JSON object，缺失或非 object 时返回空 dict。"""

        if not path.is_file():
            return {}
        payload = load_json(path)
        return payload if isinstance(payload, dict) else {}

    @classmethod
    def from_spec(cls, spec: ModelRunSpec) -> "ModelRunArtifacts":
        return cls(spec.output_dir)
