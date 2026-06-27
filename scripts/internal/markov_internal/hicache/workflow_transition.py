"""Transition exactness stage for HiCache validation workflows."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..common.io import write_json
from .transition_matrix import compare_transition_matrix
from .workflow_progress import print_stage_start


@dataclass(frozen=True)
class TransitionOptions:
    """Transition stage options independent from argparse."""

    page_key_mode: str = "strip_scope"
    force: bool = False
    sample_limit: int = 20
    emit_catalog: bool = False
    emit_gates: bool = False
    dry_run: bool = False


def run_transition_stage(output_dir: Path, options: TransitionOptions) -> dict[str, Any]:
    """运行 transition exactness 矩阵比较。"""

    if options.dry_run:
        print_stage_start("transition", "dry run: no transition comparisons will be executed")
        summary = {
            "schema": "trace_sim.hicache.transition_exactness_matrix.v1",
            "matrix_dir": str(output_dir),
            "dry_run": True,
            "prediction_count": 0,
            "ready_count": 0,
            "exact_count": 0,
        }
        write_json(output_dir / "transition_exactness_matrix.json", summary)
        return summary

    summary = compare_transition_matrix(
        output_dir,
        page_key_mode=options.page_key_mode,
        force=options.force,
        sample_limit=options.sample_limit,
        emit_catalog=options.emit_catalog,
        emit_gates=options.emit_gates,
        catalog_output=None,
        gate_output=None,
        matrix_output_path=output_dir / "transition_exactness_matrix.json",
        progress=True,
    )
    write_json(output_dir / "transition_exactness_matrix.json", summary)
    return summary
