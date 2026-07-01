"""HiCache validation workflow 的运行上下文与 artifact 布局。"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..stages.final_state import FinalStateOptions
from ..output.progress import WorkflowProgressReporter
from ..stages.transition import TransitionOptions


@dataclass(frozen=True)
class WorkflowArtifactPolicy:
    """workflow 输出目录的集中 artifact policy。"""

    output_dir: Path

    @property
    def stages_dir(self) -> Path:
        """返回阶段 summary 根目录。"""

        return self.output_dir / "stages"

    @property
    def artifacts_dir(self) -> Path:
        """返回非首屏 artifact 根目录。"""

        return self.output_dir / "artifacts"

    @property
    def quality_audit_dir(self) -> Path:
        """返回 per-run HiCache audit artifact 目录。"""

        return self.artifacts_dir / "quality"

    @property
    def runner_config_dir(self) -> Path:
        """返回 Python runner config 目录。"""

        return self.artifacts_dir / "runner_configs"

    @property
    def matrix_plan_path(self) -> Path:
        """返回 workflow matrix plan 路径。"""

        return self.artifacts_dir / "matrix_plan.json"

    @property
    def quality_summary_path(self) -> Path:
        """返回 quality stage summary 路径。"""

        return self.stages_dir / "quality" / "summary.json"

    @property
    def final_state_self_summary_path(self) -> Path:
        """返回 final-state self summary 路径。"""

        return self.stages_dir / "final_state" / "self_summary.json"

    @property
    def final_state_cross_summary_path(self) -> Path:
        """返回 final-state cross summary 路径。"""

        return self.stages_dir / "final_state" / "cross_summary.json"

    @property
    def transition_summary_path(self) -> Path:
        """返回 transition stage summary 路径。"""

        return self.stages_dir / "transition" / "summary.json"

    @property
    def transition_catalog_dir(self) -> Path:
        """返回 transition catalog artifact 目录。"""

        return self.artifacts_dir / "transition_catalog"

    def ensure_base_dirs(self) -> None:
        """创建稳定的顶层输出目录。"""

        for path in (
            self.stages_dir,
            self.artifacts_dir,
            self.quality_audit_dir,
            self.runner_config_dir,
            self.transition_catalog_dir,
        ):
            path.mkdir(parents=True, exist_ok=True)


@dataclass
class WorkflowRunContext:
    """传给 workflow stage runner 的共享运行状态。"""

    runs: list[Any]
    output_dir: Path
    stages: set[str]
    prediction_scope: set[str]
    final_state_options: FinalStateOptions
    transition_options: TransitionOptions
    artifacts: WorkflowArtifactPolicy
    reporter: WorkflowProgressReporter
    show_workload_sequence: bool = False
