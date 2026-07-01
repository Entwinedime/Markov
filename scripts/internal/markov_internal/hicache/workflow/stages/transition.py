"""HiCache validation workflow 的 transition exactness 阶段。"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

from ....common.io import write_json
from ...transition.validation.matrix import compare_transition_matrix


@dataclass(frozen=True)
class TransitionOptions:
    """与 argparse 解耦的 transition 阶段选项。"""

    page_key_mode: str = "strip_scope"
    force: bool = False
    sample_limit: int = 20
    emit_catalog: bool = False
    emit_gates: bool = False
    dry_run: bool = False


def run_transition_stage(
    output_dir: Path,
    options: TransitionOptions,
    *,
    summary_path: Path,
    catalog_dir: Path,
    on_row: Callable[[dict[str, Any]], None] | None = None,
) -> dict[str, Any]:
    """运行 transition exactness 矩阵比较。"""

    if options.dry_run:
        summary = {
            "schema": "trace_sim.hicache.transition_exactness_matrix.v1",
            "matrix_dir": str(output_dir),
            "dry_run": True,
            "prediction_count": 0,
            "ready_count": 0,
            "exact_count": 0,
        }
        write_json(summary_path, summary)
        return summary

    summary = compare_transition_matrix(
        output_dir,
        page_key_mode=options.page_key_mode,
        force=options.force,
        sample_limit=options.sample_limit,
        emit_catalog=options.emit_catalog,
        emit_gates=options.emit_gates,
        catalog_output=catalog_dir / "transition_mismatch_catalog.json",
        gate_output=catalog_dir / "transition_patch_gate_scoreboard.json",
        matrix_output_path=summary_path,
        on_row=on_row,
    )
    write_json(summary_path, summary)
    return summary
