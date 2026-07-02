"""统一 modeling workflow 的核心数据类型。"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from .artifacts import ModelRunArtifacts


class ModelOutputRequirement(str, Enum):
    """一次 C++ modeling 执行需要产出的语义结果。"""

    BASE_DAG_DIAGNOSTICS = "base_dag_diagnostics"
    MODULE_SUMMARY = "module_summary"
    MODULE_VALIDATION = "module_validation"
    MODULE_TRANSITION_DIAGNOSTICS = "module_transition_diagnostics"
    CHROME_TRACE = "chrome_trace"


@dataclass(frozen=True)
class ProfileRunRef:
    """workflow 选中的一次真实 profiling run。"""

    manifest_path: Path
    run_dir: Path
    config_path: Path
    run_id: str
    config_id: str
    input_id: str
    input_class: str
    python_probe_files: tuple[Path, ...]
    hicache_config: dict[str, Any] | None = None

    @property
    def label(self) -> str:
        return f"{self.input_id}/{self.config_id}"


@dataclass(frozen=True)
class CacheStatePredictionRef:
    """把 source profile 放到 target config/oracle 下重放的 prediction。"""

    source: ProfileRunRef
    target: ProfileRunRef

    @property
    def input_id(self) -> str:
        return self.source.input_id

    @property
    def is_self(self) -> bool:
        return self.source.config_id == self.target.config_id

    @property
    def label(self) -> str:
        return f"{self.input_id}/{self.source.config_id}->{self.target.config_id}"


@dataclass(frozen=True)
class ModelRunSpec:
    """validation requests 合并后的一次 C++ modeling 执行计划。"""

    run_id: str
    mode: str
    source_profile: ProfileRunRef
    target_profile: ProfileRunRef | None
    output_requirements: frozenset[ModelOutputRequirement]
    validation_requests: tuple[str, ...]
    output_dir: Path
    prediction: CacheStatePredictionRef | None = None
    skip_reason: str = ""

    @property
    def label(self) -> str:
        if self.prediction is not None:
            return self.prediction.label
        return self.source_profile.label


@dataclass(frozen=True)
class ModelRunResult:
    """一次 ModelRunSpec 执行或复用后的结果。"""

    spec: ModelRunSpec
    return_code: int
    elapsed_sec: float
    artifacts: "ModelRunArtifacts"
    skipped: bool = False
    dry_run: bool = False
    skip_reason: str = ""
    execution_error_tail: str = ""

    @property
    def ok(self) -> bool:
        return self.return_code == 0


@dataclass
class ValidationSummary:
    """单个 validation object 输出的紧凑摘要。"""

    name: str
    status: str
    selected_run_count: int
    ready_count: int = 0
    exact_count: int | None = None
    skipped_count: int = 0
    blocker_counts: dict[str, int] = field(default_factory=dict)
    artifact_paths: dict[str, str] = field(default_factory=dict)
    payload: dict[str, Any] = field(default_factory=dict)
