"""Build the one fixed-calibration HiCache service/control/phase model."""

from __future__ import annotations

import argparse
import math
from pathlib import Path
from statistics import median
from typing import Any

from ..common.io import load_json, write_json
from ..common.paths import require_repo_path
from .control_cost import control_models
from .coverage import scan_model_inputs
from .io_model import HiCacheIoModel
from .io_model_contract import OPERATION_KINDS, io_observation_ready
from .io_model_validation import required_resource_lanes
from .phase_calibration import build_phase_cost


def _log_interpolate(points: list[dict[str, Any]], coordinate: float, coordinate_field: str,
                     value_field: str, *, log_value: bool = True) -> float:
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
    raise RuntimeError("physical calibration interpolation failed")


def _new_parameters(points: list[dict[str, Any]], page_bytes: float) -> tuple[float, float]:
    return (
        _log_interpolate(points, page_bytes, "page_bytes", "setup_us_per_operation", log_value=False),
        _log_interpolate(points, page_bytes, "page_bytes", "bandwidth_bytes_per_sec"),
    )


def _existing_bandwidth(points: list[dict[str, Any]], page_bytes: float, operation_pages: float) -> float:
    by_page: dict[int, list[dict[str, Any]]] = {}
    for point in points:
        by_page.setdefault(int(point["page_bytes"]), []).append(point)
    curve = [
        {
            "page_bytes": calibrated_page,
            "bandwidth_bytes_per_sec": _log_interpolate(
                values, operation_pages, "operation_pages", "bandwidth_bytes_per_sec"
            ),
        }
        for calibrated_page, values in sorted(by_page.items())
    ]
    return _log_interpolate(curve, page_bytes, "page_bytes", "bandwidth_bytes_per_sec")


def _h2s_projection(observed: dict[str, Any], service: dict[str, Any], page_bytes: float) -> tuple[float, float]:
    existing = 0.0
    new = 0.0
    for batch in observed["storage_service_batches"]:
        existing_pages = int(batch["storage_existing_page_count"])
        new_pages = int(batch["storage_new_page_count"])
        if existing_pages:
            bandwidth = _existing_bandwidth(service["existing_key_bandwidth_points"], page_bytes, batch["page_count"])
            existing += existing_pages * page_bytes * 1e6 / bandwidth
        if new_pages:
            setup, bandwidth = _new_parameters(service["new_operation_points"], page_bytes)
            new += setup + new_pages * page_bytes * 1e6 / bandwidth
    return existing, new


def _physical_service(observed: dict[str, Any], services: dict[str, Any], geometry: int) -> dict[str, Any] | None:
    kind = observed.get("kind")
    if kind not in OPERATION_KINDS or not io_observation_ready(observed) or not observed["service_observed"]:
        return None
    pages = int(observed["service_page_count"])
    service_us = float(observed.get("service_us") or 0.0)
    page_size = int(observed.get("page_size") or 0)
    if pages <= 0 or service_us <= 0 or page_size <= 0:
        return None
    page_bytes = page_size * geometry
    byte_count = pages * page_bytes
    model = services[kind]
    existing = new = 0.0
    if kind == "prefetch":
        calls = len(observed["storage_service_batches"])
        if calls == 0:
            return None
        physical_us = (
            calls * float(model.get("setup_us_per_operation") or 0.0)
            + pages * float(model.get("setup_us_per_page") or 0.0)
            + byte_count * 1e6 / float(model["bandwidth_bytes_per_sec"])
        )
    elif kind in {"load", "write_device_to_host"}:
        bandwidth = _log_interpolate(model["page_bandwidth_points"], page_bytes, "page_bytes", "bandwidth_bytes_per_sec")
        setup = _log_interpolate(model["page_bandwidth_points"], page_bytes, "page_bytes", "setup_us_per_operation", log_value=False)
        physical_us = setup * observed["operation_count"] + byte_count * 1e6 / bandwidth
    else:
        if not observed.get("storage_residency_observed") or not observed.get("storage_service_batches"):
            return None
        existing, new = _h2s_projection(observed, model, page_bytes)
        physical_us = existing + new
    if physical_us <= 0:
        return None
    if kind in {"prefetch", "write_host_to_storage"}:
        service_call_bytes = [
            int(batch["page_count"]) * page_bytes
            for batch in observed.get("storage_service_batches", [])
        ]
    else:
        operation_count = int(observed.get("operation_count") or 0)
        service_call_bytes = [byte_count / operation_count] * operation_count if operation_count > 0 else []
    return {
        "family": kind,
        "source_manifest": None,
        "observed_service_us": service_us,
        "physical_service_us": physical_us,
        "physical_existing_us": existing,
        "physical_new_us": new,
        "page_size": page_size,
        "page_count": pages,
        "byte_count": byte_count,
        "storage_new_batch_pages": [
            int(batch.get("storage_new_page_count") or 0)
            for batch in observed.get("storage_service_batches", [])
            if int(batch.get("storage_new_page_count") or 0) > 0
        ],
        "service_call_bytes": service_call_bytes,
    }


def service_observation_rows(physical: dict[str, Any], captures: list[dict[str, Any]]) -> list[dict[str, Any]]:
    services = physical["service_models"]
    geometry = int(physical["kv_geometry"]["kv_bytes_per_token_per_rank"])
    rows = []
    for capture in captures:
        for observed in capture["source_io_observations"]["observations"]:
            row = _physical_service(observed, services, geometry)
            if row is not None:
                row["source_manifest"] = capture["source_manifest"]
                row["role"] = capture["role"]
                rows.append(row)
    return rows


def _runtime_scale_curve(
    rows: list[dict[str, Any]], family: str, geometry: int, *, storage_state: str | None = None
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    selected = [row for row in rows if row["family"] == family]
    physical_field = "physical_service_us"
    if storage_state is not None:
        other = "new" if storage_state == "existing" else "existing"
        selected = [
            row
            for row in selected
            if row[f"physical_{storage_state}_us"] > 0 and row[f"physical_{other}_us"] == 0
        ]
        physical_field = f"physical_{storage_state}_us"
    by_capture: dict[str, list[dict[str, Any]]] = {}
    for row in selected:
        by_capture.setdefault(row["source_manifest"], []).append(row)
    details = []
    for source, values in sorted(by_capture.items()):
        observed = math.fsum(row["observed_service_us"] for row in values)
        physical = math.fsum(row[physical_field] for row in values)
        if observed <= 0 or physical <= 0:
            continue
        ratio = observed / physical
        details.append({"source_manifest": source, "role": values[0]["role"], "page_size": values[0]["page_size"],
                        "operation_count": len(values),
                        "observed_service_us": observed, "physical_service_us": physical,
                        "runtime_scale": ratio})
    if not details:
        suffix = f" {storage_state}" if storage_state else ""
        raise ValueError(f"base plus fixed calibration did not identify {family}{suffix} service")
    points = []
    anchors_by_page: dict[int, list[dict[str, Any]]] = {}
    for row in details:
        anchors_by_page.setdefault(row["page_size"], []).append(row)
    for page_size, page_rows in sorted(anchors_by_page.items()):
        points.append({
            "page_bytes": page_size * geometry,
            "runtime_scale": median(row["runtime_scale"] for row in page_rows),
            "source_manifests": [row["source_manifest"] for row in page_rows],
            "repeat_count": len(page_rows),
        })
    observed_total = math.fsum(row["observed_service_us"] for row in selected)
    physical_total = math.fsum(row[physical_field] for row in selected)
    actual = [row["observed_service_us"] for row in selected]
    predicted = [
        row[physical_field] * _log_interpolate(points, row["page_size"] * geometry, "page_bytes", "runtime_scale")
        for row in selected
    ]
    model_points = [{"page_bytes": row["page_bytes"], "runtime_scale": row["runtime_scale"]} for row in points]
    return model_points, {
        "formula": (
            "at each fixed page endpoint: median repeat observed/physical scale; "
            "log interpolation between the two endpoint anchors"
        ),
        "unit": "dimensionless",
        "capture_count": len(details),
        "page_anchor_count": len(points),
        "operation_count": len(selected),
        "observed_service_us": observed_total,
        "physical_service_us": physical_total,
        "per_capture": details,
        "points": points,
        "input_reconstruction_wape": sum(abs(a - p) for a, p in zip(actual, predicted)) / observed_total,
    }


def _new_write_curve(rows: list[dict[str, Any]], geometry: int) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Identify per-service-call setup and bytes cost from pure-new operations.

    Divide both measured time and bytes by the number of calls before solving
    the two anchors. This preserves T = calls * setup + bytes / bandwidth even
    when an operation spans several (possibly unequal) storage batches.
    """

    selected = [row for row in rows if row["family"] == "write_host_to_storage"
                and row["physical_new_us"] > 0 and row["physical_existing_us"] == 0]
    by_capture: dict[str, list[dict[str, Any]]] = {}
    for row in selected:
        by_capture.setdefault(row["source_manifest"], []).append(row)
    fits = []
    for source, values in sorted(by_capture.items()):
        samples: dict[float, list[float]] = {}
        for row in values:
            calls = len(row["storage_new_batch_pages"])
            samples.setdefault(row["byte_count"] / calls, []).append(row["observed_service_us"] / calls)
        anchors = [{"bytes_per_call": size, "duration_us_per_call": median(times), "sample_count": len(times)}
                   for size, times in sorted(samples.items())]
        if len(anchors) < 2:
            raise ValueError("fixed calibration needs two new-write bytes-per-call sizes at every page endpoint")
        short, long = anchors[0], anchors[-1]
        slope = (long["duration_us_per_call"] - short["duration_us_per_call"]) / (
            long["bytes_per_call"] - short["bytes_per_call"]
        )
        setup = short["duration_us_per_call"] - slope * short["bytes_per_call"]
        if setup < 0 or slope <= 0:
            raise ValueError("fixed calibration did not identify non-negative new-write setup and bandwidth")
        fits.append({
            "source_manifest": source,
            "page_size": values[0]["page_size"],
            "setup_us_per_operation": setup,
            "bandwidth_bytes_per_sec": 1_000_000.0 / slope,
            "fit_anchors": [short, long],
            "observed_anchors": anchors,
        })
    points = []
    for page_size in sorted({row["page_size"] for row in fits}):
        repeats = [row for row in fits if row["page_size"] == page_size]
        points.append({
            "page_bytes": page_size * geometry,
            "setup_us_per_operation": median(row["setup_us_per_operation"] for row in repeats),
            "bandwidth_bytes_per_sec": median(row["bandwidth_bytes_per_sec"] for row in repeats),
        })
    if len(points) < 2:
        raise ValueError("fixed calibration did not cover both new-write page endpoints")
    return points, {
        "formula": "per endpoint: operation duration = new_service_calls * setup_per_call + bytes / sustained_bandwidth",
        "parameter_method": "normalize each operation by service-call count; solve two size anchors; median across repeats",
        "units": {"setup_us_per_operation": "microseconds", "bandwidth_bytes_per_sec": "bytes/second"},
        "per_capture": fits,
        "points": points,
    }


def _service_model(
    family: str, physical: dict[str, Any], scales: dict[str, list[dict[str, Any]]], new_write: list[dict[str, Any]]
) -> dict[str, Any]:
    fields = {"direction": physical["direction"]}
    if family == "prefetch":
        fields.update(
            setup_us_per_operation=physical["setup_us_per_operation"],
            setup_us_per_page=physical["setup_us_per_page"],
            bandwidth_bytes_per_sec=physical["bandwidth_bytes_per_sec"],
            runtime_scale_points=scales[family],
        )
    elif family in {"load", "write_device_to_host"}:
        fields["page_bandwidth_points"] = physical["page_bandwidth_points"]
        fields["runtime_scale_points"] = scales[family]
    else:
        fields.update(
            new_operation_points=new_write,
            existing_key_bandwidth_points=physical["existing_key_bandwidth_points"],
            existing_runtime_scale_points=scales["write_host_to_storage_existing"],
        )
    return fields


def _row_prediction(
    row: dict[str, Any], scales: dict[str, list[dict[str, Any]]], geometry: int,
    new_write: list[dict[str, Any]],
) -> float:
    page_bytes = row["page_size"] * geometry
    if row["family"] != "write_host_to_storage":
        scale = _log_interpolate(scales[row["family"]], page_bytes, "page_bytes", "runtime_scale")
        return row["physical_service_us"] * scale
    existing_scale = _log_interpolate(
        scales["write_host_to_storage_existing"], page_bytes, "page_bytes", "runtime_scale"
    )
    setup, bandwidth = _new_parameters(new_write, page_bytes)
    new_service = sum(setup + pages * page_bytes * 1e6 / bandwidth for pages in row["storage_new_batch_pages"])
    return row["physical_existing_us"] * existing_scale + new_service


def _reconstruction(
    rows: list[dict[str, Any]], scales: dict[str, list[dict[str, Any]]], geometry: int,
    new_write: list[dict[str, Any]],
) -> dict[str, Any]:
    result = {}
    for family in OPERATION_KINDS:
        values = [row for row in rows if row["family"] == family]
        actual = sum(row["observed_service_us"] for row in values)
        predicted = [_row_prediction(row, scales, geometry, new_write) for row in values]
        error = sum(abs(row["observed_service_us"] - estimate) for row, estimate in zip(values, predicted))
        result[family] = {
            "operation_count": len(values),
            "observed_service_us": actual,
            "predicted_service_us": sum(predicted),
            "wape": error / actual if actual else None,
        }
    return result


def _io_coverage(rows: list[dict[str, Any]]) -> dict[str, Any]:
    """Describe observed calibration support without changing model behavior."""

    page_bytes = sorted({int(row["byte_count"] / row["page_count"]) for row in rows if row["page_count"] > 0})
    calls: dict[str, dict[str, Any]] = {}
    for family in OPERATION_KINDS:
        values = [float(value) for row in rows if row["family"] == family for value in row["service_call_bytes"] if value > 0]
        if values:
            calls[family] = {
                "min": min(values),
                "max": max(values),
                "sample_count": len(values),
            }
    return {
        "source": "fixed_calibration_observations",
        "page_bytes": {"min": min(page_bytes), "max": max(page_bytes), "anchors": page_bytes},
        "service_call_bytes": calls,
        "outside_domain_behavior": "page endpoint clamp; service-call size remains a physical-model projection and is reported unverified",
    }


def build_io_model(physical: dict[str, Any], captures: list[dict[str, Any]], base_page_size: int) -> tuple[dict, dict, dict]:
    unique = {capture["source_manifest"]: capture for capture in captures}
    fixed = [capture for capture in unique.values() if capture["role"] == "calibration"]
    bases = [capture for capture in unique.values() if capture["role"] == "base"]
    captures = [*bases, *fixed]
    model_rows = service_observation_rows(physical, fixed)
    geometry = int(physical["kv_geometry"]["kv_bytes_per_token_per_rank"])
    scales = {}
    service_sources = {}
    for family in OPERATION_KINDS[:-1]:
        scales[family], service_sources[family] = _runtime_scale_curve(model_rows, family, geometry)
    scales["write_host_to_storage_existing"], service_sources["write_host_to_storage_existing"] = (
        _runtime_scale_curve(model_rows, "write_host_to_storage", geometry, storage_state="existing")
    )
    new_write, service_sources["write_host_to_storage_new"] = _new_write_curve(model_rows, geometry)
    controls, control_sources = control_models(fixed)
    phase, phase_summary = build_phase_cost(captures, base_page_size)
    physical_services = physical["service_models"]
    model = {
        "storage_batch_pages": physical["storage_batch_pages"],
        "kv_bytes_per_token_per_rank": geometry,
        "service_models": {
            family: _service_model(family, physical_services[family], scales, new_write) for family in OPERATION_KINDS
        },
        "control_models": controls,
        "resource_lanes": required_resource_lanes(physical.get("resource_lanes")),
        "phase_cost": phase,
        "io_coverage": _io_coverage(model_rows),
    }
    normalized = HiCacheIoModel.from_raw(Path("hicache_io_model.json"), model).fields
    summary = {
        "status": "ready",
        "model_form": (
            "physical service times fixed endpoint scales; new H2S uses directly measured setup plus bandwidth"
        ),
        "parameter_sources": {
            "physical": "explicit platform calibration",
            "service_runtime_scales": service_sources,
            "control": control_sources,
            "phase": phase_summary["parameter_sources"],
        },
        "platform_measurement": {
            "description": physical.get("measurement_description"),
            "sources": list(physical.get("measurement_sources") or []),
            "scope": physical.get("measurement_scope") or {},
        },
        "observation_sources": {
            "base": [capture["source_manifest"] for capture in bases],
            "fixed_calibration": [capture["source_manifest"] for capture in fixed],
        },
        "direct_input_reconstruction": {
            "base": _reconstruction(service_observation_rows(physical, bases), scales, geometry, new_write),
            "fixed_calibration": _reconstruction(service_observation_rows(physical, fixed), scales, geometry, new_write),
        },
        "phase": phase_summary,
        "io_coverage": model["io_coverage"],
        "target_inputs": [],
        "target_score_inputs": [],
        "accuracy_verified": False,
    }
    return normalized, summary, {"phase_cost": phase, "summary": phase_summary}


def main(argv: list[str] | None = None) -> int:
    from .group import GroupRequest
    from .observations import group_observations

    parser = argparse.ArgumentParser(description="Build one base's fixed-calibration HiCache model.")
    parser.add_argument("--group", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args(argv)
    group = GroupRequest.load(require_repo_path(args.group))
    output = require_repo_path(args.output_dir) if args.output_dir else group.output_dir
    captures = group_observations(group)
    readiness = scan_model_inputs(group, captures)
    output.mkdir(parents=True, exist_ok=True)
    if readiness["status"] != "ready":
        write_json(output / "model_build_summary.json", {"status": "needs_calibration_data", "model_inputs": readiness})
        print(f"model not built: {len(readiness['missing'])} fixed-input requirements remain")
        return 2
    model, summary, phase = build_io_model(
        group.physical,
        captures,
        int(group.sources[0].hicache_config["page_size"]),
    )
    summary.update(base_group=group.base_config, model_inputs=readiness)
    write_json(output / "phase_calibration.json", phase)
    write_json(output / "hicache_io_model.json", model)
    write_json(output / "model_build_summary.json", summary)
    group_summary = group.output_dir / "group_summary.json"
    if output == group.output_dir and group_summary.exists():
        state = load_json(group_summary)
        state.update(status="model_built", predictions_completed=0, accuracy_verified=False)
        write_json(group_summary, state)
    print(f"model={output / 'hicache_io_model.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
