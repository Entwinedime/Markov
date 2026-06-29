"""HiCache 矩阵共享类型。"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class ProfileRun:
    """矩阵中的一次真实 profiling run。"""

    manifest_path: Path
    run_dir: Path
    config_path: Path
    run_id: str
    config_id: str
    input_id: str
    input_class: str
    hicache_config: dict[str, Any]
    python_probe_files: tuple[Path, ...]

    @property
    def prediction_slug(self) -> str:
        """用于输出目录名的稳定短标签。"""

        return f"{safe_slug(self.config_id)}__{safe_slug(self.input_id)}"


@dataclass(frozen=True)
class PredictionSpec:
    """一个 source profile 到 target config/oracle 的 state prediction。"""

    source: ProfileRun
    target: ProfileRun

    @property
    def input_id(self) -> str:
        """当前 prediction 所属 workload input。"""

        return self.source.input_id

    @property
    def is_self(self) -> bool:
        """是否为同配置 self prediction。"""

        return self.source.config_id == self.target.config_id

    @property
    def label(self) -> str:
        """矩阵格子的稳定标签。"""

        return f"{self.input_id}/{self.source.config_id}->{self.target.config_id}"


def safe_slug(value: str) -> str:
    """把矩阵 id 转成可用作文件/目录名的 slug。"""

    return re.sub(r"[^A-Za-z0-9_.-]+", "_", value.strip()).strip("._") or "unknown"
