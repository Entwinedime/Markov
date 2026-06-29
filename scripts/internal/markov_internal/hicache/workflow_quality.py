"""HiCache validation workflow 的 quality 阶段封装。"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Callable

from .matrix_quality import build_quality_report


def run_quality_stage(
    runs: list[Any],
    output_dir: Path,
    *,
    selected: bool,
    audit_dir: Path | None = None,
    summary_path: Path | None = None,
    on_row: Callable[[dict[str, Any]], None] | None = None,
) -> dict[str, Any]:
    """构造 quality report，但不直接负责终端输出。"""

    _ = selected
    return build_quality_report(
        runs,
        output_dir,
        audit_dir=audit_dir,
        summary_path=summary_path,
        on_row=on_row,
    )
