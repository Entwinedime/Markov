"""HiCache transition validation path helpers."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from ..common.paths import ROOT_DIR


@dataclass(frozen=True)
class PathsForPrediction:
    """一个 prediction 输出目录内的标准产物路径。"""

    prediction_dir: Path
    predicted_trace: Path
    validation: Path
    model_self_check: Path


def resolve_repo_path(path: Path) -> Path:
    """解析 repo 相对路径。"""

    path = path.expanduser()
    if path.is_absolute():
        return map_repo_path(path)
    return ROOT_DIR / path


def map_repo_path(path: Path) -> Path:
    """把容器内仓库前缀映射为当前 workspace。"""

    raw = str(path)
    for prefix in ("/workspace/trace-sim", "/opt/trace-sim"):
        if raw == prefix:
            return ROOT_DIR
        if raw.startswith(prefix + "/"):
            return ROOT_DIR / raw[len(prefix) + 1 :]
    return path


def resolve_required_path(path: Path | None, flag_name: str) -> Path:
    """解析必需路径参数。"""

    if path is None:
        raise SystemExit(f"missing required {flag_name}")
    return resolve_repo_path(path)


def resolve_output(path: Path | None, default: Path) -> Path:
    """解析输出路径。"""

    return resolve_repo_path(path) if path is not None else default
