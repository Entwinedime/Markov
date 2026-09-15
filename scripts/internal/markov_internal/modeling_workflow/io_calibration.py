"""Capture the compact, target-independent HiCache physical calibration."""

from __future__ import annotations

from pathlib import Path
from dataclasses import asdict
import time
from typing import Any

from ..common.paths import repo_relative_path, require_repo_path
from .calibration.aggregation import _is_sustained_new_write_point, select_point_durations
from .calibration.bundle import write_final_capture_bundle
from .calibration.host_storage import (
    HostStorageCapturePlan,
    capture_host_storage,
    storage_operation_byte_anchors,
)
from .calibration.options import (
    CalibrationOptions,
    format_cpu_set,
    parse_cpu_sets,
    parse_args,
    parse_positive_csv,
)
from .calibration.runtime_anchors import load_runtime_anchor_projection
from .physical_calibration import derive_kv_geometry, filesystem_type


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    result = capture_final_bundle(args, require_repo_path(args.output_dir).resolve())
    print(f"calibration_report={result['report_path']}")
    for stage, count in result["sample_counts"].items():
        print(f"{stage}_samples_run={count}")
    return 0


def capture_final_bundle(args: CalibrationOptions, output_dir: Path) -> dict[str, Any]:
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

    geometry = derive_kv_geometry(
        require_repo_path(args.model_config),
        args.tensor_parallel_size,
        args.kv_element_bytes,
    )
    page_tokens = parse_positive_csv(args.page_token_sizes, "page-token-sizes")
    page_bytes = sorted({geometry["kv_bytes_per_token_per_rank"] * value for value in page_tokens})
    runtime = load_runtime_anchor_projection(
        args.runtime_dma_report,
        expected_kv_bytes_per_token_per_rank=geometry["kv_bytes_per_token_per_rank"],
        expected_page_bytes=page_bytes,
        expected_concurrent_scope_count=args.tensor_parallel_size,
    )
    existing_pages = sorted(
        parse_positive_csv(args.storage_existing_operation_pages, "storage-existing-operation-pages")
    )
    if len(existing_pages) < 2:
        raise ValueError("existing-key calibration requires at least two operation-page anchors")
    new_queues = sorted(
        parse_positive_csv(
            args.storage_new_write_queue_bytes_per_scope,
            "storage-new-write-queue-bytes-per-scope",
        )
    )
    if any(value % page for value in new_queues for page in page_bytes):
        raise ValueError("new-write queue anchors must be divisible by every page size")
    new_operations = storage_operation_byte_anchors(
        args.storage_new_write_operation_bytes_per_scope,
        burst_bytes_per_scope=args.burst_bytes_per_scope,
        page_sizes=page_bytes,
    )
    if max(new_operations) > min(new_queues):
        raise ValueError("new-write operation anchors must fit every queue anchor")
    cpu_sets = parse_cpu_sets(args.storage_scope_cpu_sets, args.tensor_parallel_size, optional=True)

    captured = capture_host_storage(
        HostStorageCapturePlan(
            storage_dir=storage_dir,
            page_sizes=tuple(page_bytes),
            devices=tuple(runtime.environment["concurrent_devices"]),
            numa_nodes=tuple(runtime.environment["numa_nodes"]),
            model_name=geometry["model_name"],
            kv_geometry=geometry,
            warmup=args.warmup,
            repeats=args.repeats,
            isolated_repeats=args.isolated_repeats,
            scope_cpu_sets=tuple(frozenset(value) if value is not None else None for value in cpu_sets),
            existing_operation_pages=tuple(existing_pages),
            new_write_queues=tuple(new_queues),
            new_write_operations=tuple(new_operations),
        ),
        observations_path=output_dir / "physical_observations.json",
    )
    storage_samples = [row for row in captured.storage_samples if not _is_sustained_new_write_point(row)]
    storage_samples.extend(captured.new_write_samples)
    selected = select_point_durations(storage_samples, args.selection_percentile)
    expected_new = len(page_bytes) * len(new_queues) * len(new_operations)
    if sum(_is_sustained_new_write_point(row) for row in selected) != expected_new:
        raise ValueError("new-write calibration grid is incomplete")

    capture = {
        "storage_batch_pages": STORAGE_BATCH_SIZE,
        "parameters": {**{key: str(value) if isinstance(value, Path) else value for key, value in asdict(args).items()},
            "page_bytes": page_bytes,
            "tensor_parallel_size": args.tensor_parallel_size,
        },
        "kv_geometry": geometry,
        "runtime_dma": {
            "concurrent_report": str(
                repo_relative_path(require_repo_path(args.runtime_dma_report).resolve())
            ),
        },
        "host_storage": {"samples": storage_samples, "selected_points": selected},
        "measurement_scope": {
            "runtime_dma": runtime.environment["runtime_semantics"],
            "storage": f"HiCacheFile on {filesystem_type(storage_dir)}",
            "storage_path": str(repo_relative_path(storage_dir)),
            "storage_scope_cpu_threads": captured.storage_samples[0]["scope_cpu_threads"],
            "storage_scope_devices": captured.storage_samples[0]["scope_devices"],
            "storage_scope_numa_nodes": captured.storage_samples[0]["scope_numa_nodes"],
            "storage_page_representation": "model_kv_dtype_page_first_direct",
            "storage_write_path": "HiCacheController._generic_page_set + MHATokenToKVPoolHost.get_data_page; CPU Tensor indices",
            "storage_scope_cpu_sets": [
                format_cpu_set(value) if value is not None else None for value in cpu_sets
            ],
        },
        "capture_wall_seconds": time.monotonic() - started,
        "calibration_workload_trace_used": False,
        "target_workload_trace_used": False,
        "target_e2e_used": False,
    }
    result = write_final_capture_bundle(capture, output_dir, force=args.force)
    result["sample_counts"] = {
        "runtime_dma": 0,
        "host_storage": len(storage_samples),
    }
    return result


if __name__ == "__main__":
    raise SystemExit(main())
