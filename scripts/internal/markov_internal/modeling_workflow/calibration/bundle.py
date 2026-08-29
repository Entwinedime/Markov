"""Persist the compact physical calibration consumed by one-base fitting."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ...common.io import write_json
from .aggregation import build_final_capture_service_models


def write_final_capture_bundle(
    capture: dict[str, Any],
    output_dir: Path,
    *,
    force: bool = False,
) -> dict[str, Path]:
    """Write coefficients only; raw samples remain temporary capture state."""

    if capture.get("target_workload_trace_used") is not False or capture.get("target_e2e_used") is not False:
        raise ValueError("physical calibration must be target-independent")
    output_path = output_dir / "calibration_report.json"
    if output_path.exists() and not force:
        raise FileExistsError(f"calibration output already exists: {output_path}")
    control = capture.get("control_primitives")
    if not isinstance(control, dict) or set(control) != {
        "prefetch_zero_payload_us_per_operation",
        "load_us_per_page",
    }:
        raise ValueError("calibration requires the two compact control primitives")
    report = {
        "kv_geometry": {
            "kv_bytes_per_token_per_rank": int(
                (capture.get("kv_geometry") or {}).get("kv_bytes_per_token_per_rank") or 0
            )
        },
        "service_models": build_final_capture_service_models(capture),
        "control_primitives": {
            "prefetch_zero_payload_us_per_operation": float(
                control["prefetch_zero_payload_us_per_operation"]
            ),
            "load_us_per_page": float(control["load_us_per_page"]),
        },
        "resource_lanes": {"storage_read": "shared", "storage_write": "scope"},
        "measurement_scope": capture.get("measurement_scope") or {},
        "calibration_workload_trace_used": bool(capture.get("calibration_workload_trace_used")),
        "target_workload_trace_used": False,
        "target_e2e_used": False,
    }
    if report["kv_geometry"]["kv_bytes_per_token_per_rank"] <= 0:
        raise ValueError("calibration KV geometry is missing")
    if any(float(value) < 0.0 for value in report["control_primitives"].values()):
        raise ValueError("control primitives must be non-negative")
    output_dir.mkdir(parents=True, exist_ok=True)
    write_json(output_path, report)
    return {"report_path": output_path}
