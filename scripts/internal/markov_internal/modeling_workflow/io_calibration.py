"""Capture the compact, target-independent HiCache physical calibration."""

from __future__ import annotations

from pathlib import Path
import time
from typing import Any

from ..common.io import load_json
from ..common.paths import require_repo_path
from .calibration.aggregation import _is_sustained_new_write_point, select_point_durations
from .calibration.bundle import complete_service_bundle
from .calibration.host_storage import (
    HostStorageCapturePlan,
    capture_host_storage,
    storage_operation_byte_anchors,
)
from .calibration.options import (
    CalibrationOptions,
    parse_cpu_sets,
    parse_args,
    parse_positive_csv,
)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    result = capture_service(args, require_repo_path(args.output_dir).resolve())
    print(f"calibration_report={result['report_path']}")
    for stage, count in result["sample_counts"].items():
        print(f"{stage}_samples_run={count}")
    return 0


def capture_service(args: CalibrationOptions, output_dir: Path) -> dict[str, Any]:
    """Measure physical services, preserving observations independently of control."""

    started = time.monotonic()
    from sglang.srt.mem_cache.hicache_storage import STORAGE_BATCH_SIZE

    output_path = output_dir / "calibration_report.json"
    if output_path.exists() and not args.force:
        raise FileExistsError(f"calibration output already exists: {output_path}")
    storage_dir = require_repo_path(args.storage_dir).resolve()
    if output_dir == storage_dir or output_dir in storage_dir.parents or storage_dir in output_dir.parents:
        raise ValueError("--output-dir and --storage-dir must be separate directory trees")
    storage_dir.mkdir(parents=True, exist_ok=True)

    base = load_json(require_repo_path(args.base_report))
    if base.get("target_workload_trace_used") is not False or base.get("target_e2e_used") is not False:
        raise ValueError("Service supplement requires independent existing physical costs")
    if base["storage_batch_pages"] != STORAGE_BATCH_SIZE:
        raise ValueError("Service supplement must preserve the runtime storage batch size")

    geometry = base["kv_geometry"]
    page_tokens = parse_positive_csv(args.page_token_sizes, "page-token-sizes")
    page_bytes = sorted({geometry["kv_bytes_per_token_per_rank"] * value for value in page_tokens})
    scope = base["measurement_scope"]
    devices, numa_nodes = scope["storage_scope_devices"], scope["storage_scope_numa_nodes"]
    if len(devices) != geometry["tensor_parallel_size"] or len(numa_nodes) != len(devices):
        raise ValueError("Service supplement placement must cover the deployment TP ranks")
    cpu_sets = parse_cpu_sets(args.storage_scope_cpu_sets, len(devices), optional=True)
    existing_pages = (
        sorted(parse_positive_csv(args.storage_existing_operation_pages, "storage-existing-operation-pages"))
        if args.service == "existing_write"
        else []
    )
    if args.service == "existing_write" and len(existing_pages) < 2:
        raise ValueError("existing-key calibration requires at least two operation-page anchors")
    new_queues = (
        []
        if args.service != "new_write"
        else sorted(
            parse_positive_csv(
                args.storage_new_write_queue_bytes_per_scope,
                "storage-new-write-queue-bytes-per-scope",
            )
        )
    )
    if any(value % page for value in new_queues for page in page_bytes):
        raise ValueError("new-write queue anchors must be divisible by every page size")
    new_operations = (
        []
        if args.service != "new_write"
        else storage_operation_byte_anchors(
            args.storage_new_write_operation_bytes_per_scope,
            burst_bytes_per_scope=args.burst_bytes_per_scope,
            page_sizes=page_bytes,
        )
    )
    if new_operations and max(new_operations) > min(new_queues):
        raise ValueError("new-write operation anchors must fit every queue anchor")
    observations = capture_host_storage(
        HostStorageCapturePlan(
            storage_dir=storage_dir,
            page_sizes=tuple(page_bytes),
            devices=tuple(devices),
            numa_nodes=tuple(numa_nodes),
            model_name=geometry["model_name"],
            kv_geometry=geometry,
            warmup=args.warmup,
            repeats=args.repeats,
            scope_cpu_sets=tuple(frozenset(value) if value is not None else None for value in cpu_sets),
            existing_operation_pages=tuple(existing_pages),
            new_write_queues=tuple(new_queues),
            new_write_operations=tuple(new_operations),
            service=args.service,
        ),
        observations_path=output_dir / "physical_observations.json",
    )
    storage_samples = observations["host_storage"]["samples"]
    selected = select_point_durations(storage_samples, args.selection_percentile)
    expected_new = len(page_bytes) * len(new_queues) * len(new_operations)
    if sum(_is_sustained_new_write_point(row) for row in selected) != expected_new:
        raise ValueError("new-write calibration grid is incomplete")
    if sum(row["resource_state"] == "existing_key" for row in selected) != len(page_bytes) * len(existing_pages):
        raise ValueError("existing-key calibration grid is incomplete")

    observations["host_storage"]["selected_points"] = selected
    result = complete_service_bundle(
        base,
        require_repo_path(args.base_report),
        observations,
        output_dir,
        time.monotonic() - started,
        service=args.service,
    )
    result["sample_counts"] = dict(
        host_storage=len(storage_samples), prefetch_service=len(observations["prefetch_service_observations"])
    )
    return result


if __name__ == "__main__":
    raise SystemExit(main())
