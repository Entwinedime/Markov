"""Quality stage wrapper for HiCache validation workflows."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from .matrix_quality import build_quality_report
from .workflow_progress import print_stage_start, print_summary, quality_stage_detail


def run_quality_stage(
    runs: list[Any],
    output_dir: Path,
    *,
    selected: bool,
) -> dict[str, Any]:
    """Build the quality report and optionally emit user-facing stage output."""

    if selected:
        print_stage_start("quality", quality_stage_detail(runs))
    report = build_quality_report(runs, output_dir, progress=selected)
    if selected:
        print_summary("quality", report)
    return report
