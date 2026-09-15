"""Persist the compact physical calibration consumed by one-base fitting."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ...common.io import write_json
from ...common.paths import repo_relative_path
from .aggregation import build_final_capture_service_models


def write_final_capture_bundle(
    capture: dict[str, Any],
    output_dir: Path,
    *,
    force: bool = False,
) -> dict[str, Path]:
    """Keep one physical report and its small, recomputable observation record."""

    if capture.get("target_workload_trace_used") is not False or capture.get("target_e2e_used") is not False:
        raise ValueError("physical calibration must be target-independent")
    output_path = output_dir / "calibration_report.json"
    if output_path.exists() and not force:
        raise FileExistsError(f"calibration output already exists: {output_path}")
    observations_path = output_dir / "physical_observations.json"
    report = {
        "storage_batch_pages": int(capture["storage_batch_pages"]),
        "kv_geometry": {
            "kv_bytes_per_token_per_rank": int(
                (capture.get("kv_geometry") or {}).get("kv_bytes_per_token_per_rank") or 0
            )
        },
        "service_models": build_final_capture_service_models(capture),
        "measurement_sources": [str(repo_relative_path(observations_path)), *capture["runtime_dma"].values()],
        # Storage calibration runs one concurrent service process per TP scope.
        # Summed work/service yields a per-scope effective rate under contention;
        # the DAG preserves each runtime controller lane without another TP factor.
        "resource_lanes": {"storage_read": "scope", "storage_write": "scope"},
        "measurement_scope": capture.get("measurement_scope") or {},
        "calibration_workload_trace_used": bool(capture.get("calibration_workload_trace_used")),
        "target_workload_trace_used": False,
        "target_e2e_used": False,
    }
    if report["kv_geometry"]["kv_bytes_per_token_per_rank"] <= 0:
        raise ValueError("calibration KV geometry is missing")
    output_dir.mkdir(parents=True, exist_ok=True)
    write_json(observations_path, capture)
    write_json(output_path, report)
    return {"report_path": output_path, "observations_path": observations_path}
