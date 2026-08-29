"""Load runtime DMA measurements and expose only the sustained page curves."""

from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ...common.io import load_json
from ...common.paths import repo_relative_path, require_repo_path


RUNTIME_DMA_OPERATOR_NAME = "npu::transfer_kv_dim_exchange"
RUNTIME_DMA_INDEX_RESIDENCY = {"host_indices": "cpu", "device_indices": "cpu"}


@dataclass(frozen=True)
class RuntimeAnchorProjection:
    service_models: dict[str, dict[str, Any]]
    report_metadata: dict[str, Any]
    environment: dict[str, Any]
    input_sample_count: int


def load_runtime_anchor_projection(
    isolated_report_path: Path,
    concurrent_report_path: Path,
    *,
    expected_kv_bytes_per_token_per_rank: int,
    expected_page_bytes: list[int],
    expected_concurrent_scope_count: int,
) -> RuntimeAnchorProjection:
    """Validate the deployed path and select one sustained rate per page size."""

    isolated_path = require_repo_path(isolated_report_path).resolve()
    concurrent_path = require_repo_path(concurrent_report_path).resolve()
    isolated = _load_report(isolated_path, "isolated")
    concurrent = _load_report(concurrent_path, "concurrent")
    _validate_runtime(isolated, expected_scope_count=1)
    _validate_runtime(concurrent, expected_scope_count=expected_concurrent_scope_count)
    _validate_geometry(isolated, concurrent, expected_kv_bytes_per_token_per_rank)

    services = {
        "load": _service_model(concurrent, "host_to_device"),
        "write_device_to_host": _service_model(concurrent, "device_to_host"),
    }
    expected_domain = sorted(set(expected_page_bytes))
    for kind, service in services.items():
        domain = [int(point["page_bytes"]) for point in service["page_bandwidth_points"]]
        if domain != expected_domain:
            raise ValueError(f"{kind} page-byte domain {domain} does not match storage {expected_domain}")

    report_metadata = {
        "isolated_report": str(repo_relative_path(isolated_path)),
        "concurrent_report": str(repo_relative_path(concurrent_path)),
    }
    return RuntimeAnchorProjection(
        service_models=services,
        report_metadata=report_metadata,
        environment={
            "operator": RUNTIME_DMA_OPERATOR_NAME,
            "service_clock": "operator_device_total_duration",
            "runtime_semantics": "kernel_ascend/page_first_direct/NUMA-local-pinned",
            "isolated_devices": list((isolated.get("parameters") or {}).get("devices") or []),
            "concurrent_devices": list((concurrent.get("parameters") or {}).get("devices") or []),
        },
        input_sample_count=len(isolated.get("samples") or []) + len(concurrent.get("samples") or []),
    )


def _service_model(report: dict[str, Any], direction: str) -> dict[str, Any]:
    samples = [
        row
        for row in report.get("samples") or []
        if isinstance(row, dict)
        and row.get("direction") == direction
        and int(row.get("operation_pages") or 0) > 1
    ]
    by_page: dict[int, list[float]] = {}
    for row in samples:
        by_page.setdefault(int(row["page_bytes"]), []).append(float(row["bandwidth_bytes_per_sec"]))
    if len(by_page) < 2:
        raise ValueError(f"concurrent runtime DMA has fewer than two sustained page points for {direction}")
    return {
        "direction": direction,
        "page_bandwidth_points": [
            {"page_bytes": page_bytes, "bandwidth_bytes_per_sec": _percentile(rates, 0.25)}
            for page_bytes, rates in sorted(by_page.items())
        ],
    }


def _percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered or not 0.0 <= fraction <= 1.0:
        raise ValueError("invalid percentile input")
    position = fraction * (len(ordered) - 1)
    lower, upper = math.floor(position), math.ceil(position)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def _load_report(path: Path, role: str) -> dict[str, Any]:
    report = load_json(path)
    if not isinstance(report, dict):
        raise ValueError(f"{role} runtime DMA report must be an object")
    if report.get("target_workload_trace_used") is not False or report.get("target_e2e_used") is not False:
        raise ValueError(f"{role} runtime DMA report is not target-independent")
    return report


def _validate_runtime(report: dict[str, Any], *, expected_scope_count: int) -> None:
    semantics = report.get("runtime_semantics") or {}
    if semantics.get("backend") != "kernel_ascend" or semantics.get("layout") != "page_first_direct":
        raise ValueError("runtime DMA report does not match kernel_ascend/page_first_direct")
    if semantics.get("operator") != RUNTIME_DMA_OPERATOR_NAME:
        raise ValueError("runtime DMA report uses a different transfer operator")
    if semantics.get("index_residency") != RUNTIME_DMA_INDEX_RESIDENCY:
        raise ValueError("runtime DMA report uses different index residency")
    if report.get("model_weights_loaded") is not False:
        raise ValueError("runtime DMA calibration must not load model weights")
    if int((report.get("parameters") or {}).get("scope_count") or 0) != expected_scope_count:
        raise ValueError(f"runtime DMA report requires scope_count={expected_scope_count}")


def _validate_geometry(isolated: dict[str, Any], concurrent: dict[str, Any], expected: int) -> None:
    isolated_bytes = int((isolated.get("geometry") or {}).get("kv_bytes_per_token_per_rank") or 0)
    concurrent_bytes = int((concurrent.get("geometry") or {}).get("kv_bytes_per_token_per_rank") or 0)
    if isolated_bytes != expected or concurrent_bytes != expected:
        raise ValueError("runtime DMA KV geometry does not match storage calibration")
