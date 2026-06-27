"""Workflow plan artifact writer for HiCache validation workflows."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ..common.io import write_json
from .matrix_discovery import group_runs_by_input
from .workflow_final_state import FinalStateOptions, prediction_specs_for_options


def write_workflow_plan(
    output_dir: Path,
    runs: list[Any],
    stages: set[str],
    final_state_options: FinalStateOptions,
) -> None:
    """写出 workflow 执行计划，同时保持 transition compare 需要的 matrix_plan 结构。"""

    grouped = group_runs_by_input(runs)
    selected_specs = prediction_specs_for_options(runs, final_state_options)
    plan = {
        "schema": "trace_sim.hicache.state_workflow.plan.v1",
        "stages": sorted(stages),
        "prediction_scope": sorted(final_state_options.prediction_scope),
        "dry_run": bool(final_state_options.dry_run),
        "force": bool(final_state_options.force),
        "run_count": len(runs),
        "config_ids": sorted({run.config_id for run in runs}),
        "input_ids": sorted(grouped),
        "prediction_count": len(selected_specs),
        "self_prediction_count": sum(1 for spec in selected_specs if spec.is_self),
        "cross_prediction_count": sum(1 for spec in selected_specs if not spec.is_self),
        "inputs": {
            input_id: {
                "config_ids": sorted(by_config),
                "run_ids": {config_id: run.run_id for config_id, run in sorted(by_config.items())},
            }
            for input_id, by_config in grouped.items()
        },
        "runs": [
            {
                "run_id": run.run_id,
                "config_id": run.config_id,
                "input_id": run.input_id,
                "input_class": run.input_class,
                "manifest_path": str(run.manifest_path),
                "hicache_config": run.hicache_config,
                "python_probe_files": [str(path) for path in run.python_probe_files],
                "python_probe_file_count": len(run.python_probe_files),
            }
            for run in runs
        ],
    }
    write_json(output_dir / "matrix_plan.json", plan)
