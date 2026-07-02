"""HiCache transition validation 路径辅助工具。"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from markov_internal.common.paths import resolve_repo_path


@dataclass(frozen=True)
class PathsForPrediction:
    """一个 prediction 输出目录内的标准产物路径。"""

    prediction_dir: Path
    predicted_trace: Path
    validation: Path
    model_self_check: Path


def resolve_required_path(path: Path | None, flag_name: str) -> Path:
    """解析必需路径参数。"""

    if path is None:
        raise SystemExit(f"missing required {flag_name}")
    resolved = resolve_repo_path(path)
    if resolved is None:
        raise SystemExit(f"missing required {flag_name}")
    return resolved


def resolve_output(path: Path | None, default: Path) -> Path:
    """解析输出路径。"""

    resolved = resolve_repo_path(path) if path is not None else default
    if resolved is None:
        raise SystemExit("internal error: output path unexpectedly empty")
    return resolved
