"""Build one compact HiCache I/O model from calibration and one base."""

from __future__ import annotations

import argparse
import math
from pathlib import Path
from typing import Any

from ..common.io import load_json, write_json
from ..common.paths import require_repo_path
from .io_model import HiCacheIoModel
from .io_model_contract import OPERATION_KINDS, rounded_positive_u64


def build_io_model(calibration: dict[str, Any], base: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
    services = calibration.get("service_models")
    cells = base.get("cells")
    if not isinstance(services, dict) or not isinstance(cells, list) or not cells:
        raise ValueError("model build requires calibration service_models and one base's observation cells")

    rows = [_observation_row(cell, kind, services) for cell in cells for kind in OPERATION_KINDS]
    by_family = {kind: [row for row in rows if row["family"] == kind] for kind in OPERATION_KINDS}
    prefetch = services["prefetch"]
    load = services["load"]
    d2h = services["write_device_to_host"]
    h2s = services["write_host_to_storage"]
    existing_scale, new_scale, h2s_rank = _h2s_scales(by_family["write_host_to_storage"])

    control = calibration.get("control_primitives") or {}
    zero_payload_control = float(control.get("prefetch_zero_payload_us_per_operation") or 0.0)
    load_page_control = float(control.get("load_us_per_page") or 0.0)
    model = {
        "kv_bytes_per_token_per_rank": (calibration.get("kv_geometry") or {}).get(
            "kv_bytes_per_token_per_rank"
        ),
        "planning_rates": {
            "device_host_bytes_per_sec": min(
                _minimum_calibration_bandwidth(load),
                _minimum_calibration_bandwidth(d2h),
            ),
            "host_storage_bytes_per_sec": min(
                _minimum_calibration_bandwidth(prefetch),
                _minimum_calibration_bandwidth(h2s),
            ),
        },
        "service_models": {
            "prefetch": {
                "direction": "storage_to_host",
                "setup_us_per_operation": float(prefetch.get("setup_us_per_operation") or 0.0),
                "setup_us_per_page": float(prefetch.get("setup_us_per_page") or 0.0),
                "bandwidth_bytes_per_sec": float(prefetch["bandwidth_bytes_per_sec"]),
                "runtime_scale": _total_scale(by_family["prefetch"]),
            },
            "load": {
                "direction": "host_to_device",
                "page_bandwidth_points": load["page_bandwidth_points"],
                "runtime_scale": _total_scale(by_family["load"]),
            },
            "write_device_to_host": {
                "direction": "device_to_host",
                "page_bandwidth_points": d2h["page_bandwidth_points"],
                "runtime_scale": _total_scale(by_family["write_device_to_host"]),
            },
            "write_host_to_storage": {
                "direction": "host_to_storage",
                "new_operation_points": h2s["new_operation_points"],
                "existing_key_bandwidth_points": h2s["existing_key_bandwidth_points"],
                "existing_runtime_scale": existing_scale,
                "new_runtime_scale": new_scale,
            },
        },
        "control_models": {
            "prefetch": {
                "fixed_us_per_operation": _fixed_control(by_family["prefetch"], 0.0),
                "zero_payload_fixed_us_per_operation": float(zero_payload_control or 0.0),
                "per_page_us": 0.0,
            },
            "load": {
                "fixed_us_per_operation": _fixed_control(by_family["load"], load_page_control),
                "zero_payload_fixed_us_per_operation": 0.0,
                "per_page_us": load_page_control,
            },
            "write_device_to_host": _zero_control(),
            "write_host_to_storage": _zero_control(),
        },
        "resource_lanes": {"storage_read": "shared", "storage_write": "scope"},
    }
    normalized = HiCacheIoModel.from_raw(Path("hicache_io_model.json"), model).fields
    summary = {
        "status": "ready",
        "row_count": len(rows),
        "workload_count": len(cells),
        "base_service_observation_count": {
            kind: sum(int(row["byte_count"]) > 0 for row in by_family[kind])
            for kind in OPERATION_KINDS
        },
        "h2s_identifiability_rank": h2s_rank,
        "planning_rates": normalized["planning_rates"],
    }
    return normalized, summary


def _minimum_calibration_bandwidth(model: dict[str, Any]) -> int:
    if "page_bandwidth_points" in model:
        values = [point["bandwidth_bytes_per_sec"] for point in model["page_bandwidth_points"]]
    elif "new_operation_points" in model:
        values = [point["bandwidth_bytes_per_sec"] for point in model["new_operation_points"]]
        values.extend(point["bandwidth_bytes_per_sec"] for point in model["existing_key_bandwidth_points"])
    else:
        values = [model["bandwidth_bytes_per_sec"]]
    return rounded_positive_u64(min(values), "calibration planning bandwidth")


def _observation_row(cell: Any, kind: str, services: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(cell, dict) or not isinstance(cell.get("by_kind"), dict):
        raise ValueError("base observation cell is missing by_kind")
    observed = cell["by_kind"].get(kind)
    if not isinstance(observed, dict):
        raise ValueError(f"base observation is missing family {kind}")
    operations = int(observed.get("operation_count") or 0)
    pages = int(observed.get("page_count") or 0)
    byte_count = int(observed.get("byte_count") or 0)
    row = {
        "family": kind,
        "operation_count": operations,
        "page_count": pages,
        "byte_count": byte_count,
        "page_bytes": byte_count // pages if pages else 0,
        "existing_page_count": int(observed.get("storage_existing_page_count") or 0),
        "new_page_count": int(observed.get("storage_new_page_count") or 0),
        "calibration_service_us": 0.0,
        "calibration_existing_us": 0.0,
        "calibration_new_us": 0.0,
        "observed_service_us": int(observed.get("service_us") or 0),
        "observed_control_us": int(observed.get("control_us") or 0),
    }
    if byte_count == 0:
        return row
    page_bytes = float(row["page_bytes"])
    if kind == "prefetch":
        service = services[kind]
        row["calibration_service_us"] = (
            operations * float(service.get("setup_us_per_operation") or 0.0)
            + pages * float(service.get("setup_us_per_page") or 0.0)
            + byte_count * 1_000_000.0 / float(service["bandwidth_bytes_per_sec"])
        )
    elif kind in {"load", "write_device_to_host"}:
        bandwidth = _log_interpolate(
            services[kind]["page_bandwidth_points"], page_bytes, "page_bytes", "bandwidth_bytes_per_sec"
        )
        row["calibration_service_us"] = byte_count * 1_000_000.0 / bandwidth
    else:
        existing, new = _h2s_projection(observed, services[kind], page_bytes)
        row["calibration_existing_us"] = existing
        row["calibration_new_us"] = new
        row["calibration_service_us"] = existing + new
    return row


def _h2s_projection(observed: dict[str, Any], service: dict[str, Any], page_bytes: float) -> tuple[float, float]:
    records = observed.get("records")
    if isinstance(records, list) and records:
        existing = new = 0.0
        for record in records:
            pages = int(record.get("page_count") or 0)
            byte_count = int(record.get("byte_count") or 0)
            if pages <= 0 or byte_count <= 0:
                continue
            record_page_bytes = byte_count / pages
            existing_pages = int(record.get("storage_existing_page_count") or 0)
            new_pages = int(record.get("storage_new_page_count") or 0)
            if existing_pages:
                bandwidth = _existing_bandwidth(service["existing_key_bandwidth_points"], record_page_bytes, existing_pages)
                existing += existing_pages * record_page_bytes * 1_000_000.0 / bandwidth
            if new_pages:
                setup, bandwidth = _new_parameters(service["new_operation_points"], record_page_bytes)
                new += int(record.get("operation_count") or 1) * setup + new_pages * record_page_bytes * 1_000_000.0 / bandwidth
        return existing, new

    operations = int(observed.get("operation_count") or 0)
    existing_pages = int(observed.get("storage_existing_page_count") or 0)
    new_pages = int(observed.get("storage_new_page_count") or 0)
    existing = 0.0
    if existing_pages:
        operation_pages = existing_pages / max(1, operations)
        bandwidth = _existing_bandwidth(service["existing_key_bandwidth_points"], page_bytes, operation_pages)
        existing = existing_pages * page_bytes * 1_000_000.0 / bandwidth
    new = 0.0
    if new_pages:
        setup, bandwidth = _new_parameters(service["new_operation_points"], page_bytes)
        new = operations * setup + new_pages * page_bytes * 1_000_000.0 / bandwidth
    return existing, new


def _total_scale(rows: list[dict[str, Any]]) -> float:
    active = [row for row in rows if int(row["byte_count"]) > 0]
    if not active:
        return 1.0
    prediction = sum(float(row["calibration_service_us"]) for row in active)
    observed = sum(float(row["observed_service_us"]) for row in active)
    if prediction <= 0.0 or observed <= 0.0:
        raise ValueError("base does not identify a positive family runtime scale")
    return observed / prediction


def _h2s_scales(rows: list[dict[str, Any]]) -> tuple[float, float, int]:
    samples = [
        (float(row["calibration_existing_us"]), float(row["calibration_new_us"]), float(row["observed_service_us"]))
        for row in rows
        if float(row["calibration_service_us"]) > 0.0
    ]
    if not samples:
        return 1.0, 1.0, 0
    a = sum(x * x for x, _, _ in samples)
    b = sum(x * z for x, z, _ in samples)
    c = sum(z * z for _, z, _ in samples)
    d = sum(x * y for x, _, y in samples)
    e = sum(z * y for _, z, y in samples)
    determinant = a * c - b * b
    rank = 2 if determinant > max(a * c, 1.0) * 1e-12 else 1
    if rank == 2:
        existing = (d * c - b * e) / determinant
        new = (a * e - b * d) / determinant
        if existing > 0.0 and new > 0.0:
            return existing, new, rank
    existing = d / a if a > 0.0 else 1.0
    new = e / c if c > 0.0 else 1.0
    if existing <= 0.0 or new <= 0.0:
        raise ValueError("base H2S observations do not identify positive existing/new scales")
    return existing, new, rank


def _fixed_control(rows: list[dict[str, Any]], per_page_us: float) -> float:
    operations = sum(int(row["operation_count"]) for row in rows if int(row["byte_count"]) > 0)
    pages = sum(int(row["page_count"]) for row in rows if int(row["byte_count"]) > 0)
    observed = sum(int(row["observed_control_us"]) for row in rows if int(row["byte_count"]) > 0)
    return max(0.0, (observed - per_page_us * pages) / operations) if operations else 0.0


def _zero_control() -> dict[str, float]:
    return {
        "fixed_us_per_operation": 0.0,
        "zero_payload_fixed_us_per_operation": 0.0,
        "per_page_us": 0.0,
    }


def _new_parameters(points: list[dict[str, Any]], page_bytes: float) -> tuple[float, float]:
    return (
        _log_interpolate(points, page_bytes, "page_bytes", "setup_us_per_operation", log_value=False),
        _log_interpolate(points, page_bytes, "page_bytes", "bandwidth_bytes_per_sec"),
    )


def _existing_bandwidth(points: list[dict[str, Any]], page_bytes: float, operation_pages: float) -> float:
    by_page: dict[int, list[dict[str, Any]]] = {}
    for point in points:
        by_page.setdefault(int(point["page_bytes"]), []).append(point)
    page_curve = [
        {
            "page_bytes": calibrated_page,
            "bandwidth_bytes_per_sec": _log_interpolate(
                curve, operation_pages, "operation_pages", "bandwidth_bytes_per_sec"
            ),
        }
        for calibrated_page, curve in sorted(by_page.items())
    ]
    return _log_interpolate(page_curve, page_bytes, "page_bytes", "bandwidth_bytes_per_sec")


def _log_interpolate(
    points: list[dict[str, Any]], coordinate: float, coordinate_field: str, value_field: str, *, log_value: bool = True
) -> float:
    ordered = sorted(points, key=lambda point: float(point[coordinate_field]))
    if coordinate <= float(ordered[0][coordinate_field]):
        return float(ordered[0][value_field])
    if coordinate >= float(ordered[-1][coordinate_field]):
        return float(ordered[-1][value_field])
    for left, right in zip(ordered, ordered[1:]):
        if coordinate > float(right[coordinate_field]):
            continue
        position = (math.log(coordinate) - math.log(float(left[coordinate_field]))) / (
            math.log(float(right[coordinate_field])) - math.log(float(left[coordinate_field]))
        )
        left_value = float(left[value_field])
        right_value = float(right[value_field])
        if log_value:
            return math.exp(math.log(left_value) + position * (math.log(right_value) - math.log(left_value)))
        return left_value + position * (right_value - left_value)
    raise RuntimeError("calibration interpolation failed")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build a compact HiCache I/O model from calibration and one base.")
    parser.add_argument("--calibration-report", required=True, type=Path)
    parser.add_argument("--base-observations", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    output_dir = require_repo_path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=False)
    model, summary = build_io_model(
        load_json(require_repo_path(args.calibration_report)),
        load_json(require_repo_path(args.base_observations)),
    )
    write_json(output_dir / "hicache_io_model.json", model)
    write_json(output_dir / "model_build_summary.json", summary)
    print(f"model={output_dir / 'hicache_io_model.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
