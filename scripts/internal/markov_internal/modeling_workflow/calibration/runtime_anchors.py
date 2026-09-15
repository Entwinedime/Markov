"""Identify DMA call setup and byte cost from independent device-clock samples."""

from __future__ import annotations

import math
from statistics import median
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ...common.io import load_json
from ...common.paths import require_repo_path


RUNTIME_DMA_OPERATOR_NAME = "npu::transfer_kv_dim_exchange"
RUNTIME_DMA_INDEX_RESIDENCY = {"host_indices": "cpu", "device_indices": "cpu"}


@dataclass(frozen=True)
class RuntimeAnchorProjection:
    service_models: dict[str, dict[str, Any]]
    environment: dict[str, Any]


def load_runtime_anchor_projection(
    concurrent_report_path: Path,
    *,
    expected_kv_bytes_per_token_per_rank: int,
    expected_page_bytes: list[int],
    expected_concurrent_scope_count: int,
) -> RuntimeAnchorProjection:
    """Validate the deployed path and retain small/large operation response."""

    concurrent_path = require_repo_path(concurrent_report_path).resolve()
    concurrent = _load_report(concurrent_path)
    _validate_runtime(concurrent, expected_scope_count=expected_concurrent_scope_count)
    if int((concurrent.get("geometry") or {}).get("kv_bytes_per_token_per_rank") or 0) != expected_kv_bytes_per_token_per_rank:
        raise ValueError("runtime DMA KV geometry does not match storage calibration")

    services = {
        "load": _service_model(concurrent, "host_to_device"),
        "write_device_to_host": _service_model(concurrent, "device_to_host"),
    }
    expected_domain = sorted(set(expected_page_bytes))
    for kind, service in services.items():
        domain = [int(point["page_bytes"]) for point in service["page_bandwidth_points"]]
        if domain != expected_domain:
            raise ValueError(f"{kind} page-byte domain {domain} does not match storage {expected_domain}")

    return RuntimeAnchorProjection(
        service_models=services,
        environment={
            "operator": RUNTIME_DMA_OPERATOR_NAME,
            "service_clock": "operator_device_total_duration",
            "runtime_semantics": "kernel_ascend/page_first_direct/pinned",
            "concurrent_devices": list((concurrent.get("parameters") or {}).get("devices") or []),
            "numa_nodes": [row.get("numa_preferred_node") for row in sorted(
                concurrent["environment"]["ranks"], key=lambda row: row["rank"])],
        },
    )


def _service_model(report: dict[str, Any], direction: str) -> dict[str, Any]:
    samples = [
        row
        for row in report.get("samples") or []
        if isinstance(row, dict)
        and row.get("direction") == direction
        and int(row.get("operation_pages") or 0) > 0
    ]
    by_page: dict[int, dict[int, list[float]]] = {}
    for row in samples:
        by_page.setdefault(int(row["page_bytes"]), {}).setdefault(int(row["bytes"]), []).append(float(row["device_duration_us"]))
    if not by_page:
        raise ValueError(f"concurrent runtime DMA has no service points for {direction}")
    points = []
    for page_bytes, sizes in sorted(by_page.items()):
        if len(sizes) < 2:
            raise ValueError(f"DMA {direction} requires two operation sizes per page size")
        anchors = [(size, median(times)) for size, times in sorted(sizes.items())]
        small, large = anchors[0], anchors[-1]
        slope = (large[1] - small[1]) / (large[0] - small[0])
        setup = small[1] - slope * small[0]
        if setup < 0:
            # A negative fixed cost is unphysical: the constrained line passes
            # through the origin. This is not selected using target accuracy.
            setup = 0.
            slope = math.fsum(size * duration for size, duration in anchors) / math.fsum(size * size for size, _ in anchors)
        if slope <= 0 or not math.isfinite(slope):
            raise ValueError(f"DMA {direction} samples do not identify positive byte cost")
        points.append({"page_bytes": page_bytes, "setup_us_per_operation": setup,
                       "bandwidth_bytes_per_sec": 1_000_000. / slope})
    return {
        "direction": direction,
        "page_bandwidth_points": points,
    }


def _load_report(path: Path) -> dict[str, Any]:
    report = load_json(path)
    if not isinstance(report, dict):
        raise ValueError("runtime DMA report must be an object")
    if report.get("target_workload_trace_used") is not False or report.get("target_e2e_used") is not False:
        raise ValueError("runtime DMA report is not target-independent")
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
    devices = report.get("parameters", {}).get("devices", [])
    ranks = sorted(report.get("environment", {}).get("ranks", []), key=lambda row: row["rank"])
    if (len(devices) != expected_scope_count or len(set(devices)) != expected_scope_count
            or any(device < 0 for device in devices)
            or [row["rank"] for row in ranks] != list(range(expected_scope_count))
            or [row["device"] for row in ranks] != devices):
        raise ValueError("runtime DMA deployment devices must match its actual rank records")
    nodes = report.get("parameters", {}).get("numa_nodes")
    if nodes is not None and (len(nodes) != expected_scope_count
            or any(node is not None and node < 0 for node in nodes)
            or [row.get("numa_preferred_node") for row in ranks] != nodes):
        raise ValueError("runtime DMA NUMA preference must match its actual rank records")
