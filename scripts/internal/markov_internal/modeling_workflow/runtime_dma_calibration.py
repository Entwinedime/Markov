"""Compact calibration for the actual Ascend HiCache DMA primitive.

The generic ``Tensor.copy_`` clock does not match SGLang's deployed
``kernel_ascend`` path.  This module executes ``transfer_kv_dim_exchange``
with MHA page-first layouts, profiles the runtime operator, and derives
normalized device-cost samples from its operator table.

No model weights, request workload, target profile, or target latency label is
opened by this calibration.
"""

from __future__ import annotations

import argparse
import csv
import math
import multiprocessing
import os
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..common.io import load_json, write_json
from ..common.paths import require_repo_path
from .calibration.runtime_anchors import (
    DMA_SERVICES,
    RUNTIME_DMA_INDEX_RESIDENCY,
    RUNTIME_DMA_OPERATOR_NAME,
    load_runtime_service_models,
)
from .calibration.options import (
    apply_numa_preference,
    format_cpu_set,
    parse_cpu_sets,
    parse_positive_csv,
)
from ..common.commands import nonnegative_int, positive_int

DEFAULT_PAGE_TOKEN_SIZES = "32,64,128"
DEFAULT_PAYLOAD_BYTES = "134217728,251658240,385875968"


@dataclass(frozen=True)
class RuntimeDmaPlan:
    """Normalized deployment and sampling inputs shared with every rank worker."""

    output_dir: Path
    devices: list[int]
    cpu_sets: list[set[int]]
    numa_nodes: list[int | None]
    page_token_sizes: list[int]
    payload_bytes: list[int]
    repeats: int
    warmup: int
    layer_count: int
    kv_heads_per_rank: int
    head_dim: int
    element_bytes: int
    profiler_level: int
    device_capacity_tokens: int | None
    base_path: Path
    base_report: dict[str, Any]
    directions: tuple[str, ...] = tuple(DMA_SERVICES)


@dataclass(frozen=True)
class DmaOperation:
    sample_index: int
    direction: str
    page_tokens: int
    page_bytes: int
    operation_pages: int
    byte_count: int
    ordinal: int


def build_operation_plan(
    *,
    page_token_sizes: list[int],
    payload_bytes: list[int],
    kv_bytes_per_token_per_rank: int,
    repeats: int,
    directions: tuple[str, ...] = tuple(DMA_SERVICES),
) -> list[DmaOperation]:
    """Build the deterministic operator order used to decode profiler rows."""

    if repeats <= 0 or kv_bytes_per_token_per_rank <= 0:
        raise ValueError("runtime DMA plan requires positive repeats and KV geometry")
    operations: list[DmaOperation] = []
    for page_tokens in sorted(page_token_sizes):
        page_bytes = page_tokens * kv_bytes_per_token_per_rank
        operation_pages = [1]
        for payload in sorted(payload_bytes):
            if payload % page_bytes:
                raise ValueError(f"payload bytes {payload} is not divisible by page bytes {page_bytes}")
            pages = payload // page_bytes
            if pages not in operation_pages:
                operation_pages.append(pages)
        for direction in directions:
            for pages in operation_pages:
                for ordinal in range(repeats):
                    operations.append(
                        DmaOperation(
                            sample_index=len(operations),
                            direction=direction,
                            page_tokens=page_tokens,
                            page_bytes=page_bytes,
                            operation_pages=pages,
                            byte_count=pages * page_bytes,
                            ordinal=ordinal,
                        )
                    )
    return operations


def parse_operator_table(
    path: Path,
    operations: list[DmaOperation],
    *,
    rank: int,
) -> list[dict[str, Any]]:
    """Map the exact target-operator row sequence back to the fixed operation plan."""

    required_columns = {
        "Name",
        "Device Total Duration(us)",
    }
    with path.open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        missing = required_columns - set(reader.fieldnames or [])
        if missing:
            raise ValueError(f"rank {rank} operator table is missing required clocks: {sorted(missing)}")
        rows = [row for row in reader if row.get("Name") == RUNTIME_DMA_OPERATOR_NAME]
    if len(rows) != len(operations):
        raise ValueError(
            f"rank {rank} profiler emitted {len(rows)} {RUNTIME_DMA_OPERATOR_NAME} rows; "
            f"expected exactly {len(operations)}"
        )
    samples: list[dict[str, Any]] = []
    for operation, row in zip(operations, rows):
        duration_us = float(row.get("Device Total Duration(us)") or 0.0)
        if not math.isfinite(duration_us) or duration_us <= 0.0:
            raise ValueError(f"rank {rank} sample {operation.sample_index} has invalid device duration")
        samples.append(
            {
                "rank": rank,
                "sample_index": operation.sample_index,
                "ordinal": operation.ordinal,
                "direction": operation.direction,
                "page_bytes": operation.page_bytes,
                "operation_pages": operation.operation_pages,
                "bytes": operation.byte_count,
                "device_duration_us": duration_us,
                "bandwidth_bytes_per_sec": operation.byte_count * 1_000_000.0 / duration_us,
            }
        )
    return samples


def _profile_rank(
    plan: RuntimeDmaPlan,
    rank: int,
    operations: list[DmaOperation],
    barrier: Any,
    result_queue: Any,
) -> None:
    device, cpu_set = plan.devices[rank], plan.cpu_sets[rank]
    try:
        os.sched_setaffinity(0, cpu_set)
        actual_affinity = sorted(os.sched_getaffinity(0))
        if set(actual_affinity) != cpu_set:
            raise RuntimeError(
                f"rank {rank} CPU affinity mismatch: requested={sorted(cpu_set)}, actual={actual_affinity}"
            )
        actual_numa_node = apply_numa_preference(plan.numa_nodes[rank])
        import torch
        import torch_npu
        from sgl_kernel_npu.kvcacheio import TransferDirection, transfer_kv_dim_exchange

        torch.npu.set_device(device)
        profile_root = plan.output_dir / "profile_work" / f"rank{rank}"
        profile_root.mkdir(parents=True, exist_ok=True)
        experimental = torch_npu.profiler._ExperimentalConfig(
            export_type=[torch_npu.profiler.ExportType.Text],
            profiler_level=getattr(torch_npu.profiler.ProfilerLevel, f"Level{plan.profiler_level}"),
        )
        profiler = torch_npu.profiler.profile(
            activities=[
                torch_npu.profiler.ProfilerActivity.CPU,
                torch_npu.profiler.ProfilerActivity.NPU,
            ],
            record_shapes=False,
            profile_memory=False,
            with_stack=False,
            on_trace_ready=torch_npu.profiler.tensorboard_trace_handler(
                str(profile_root),
                async_mode=False,
            ),
            experimental_config=experimental,
        )

        groups: dict[int, list[DmaOperation]] = {}
        for operation in operations:
            groups.setdefault(operation.page_tokens, []).append(operation)
        tensors: dict[int, tuple[Any, ...]] = {}
        layouts = []
        for page_tokens, group in groups.items():
            max_pages = max(operation.operation_pages for operation in group)
            device_pages = (
                max_pages if plan.device_capacity_tokens is None else plan.device_capacity_tokens // page_tokens
            )
            # These are the deployed NPU MHA layouts:
            # device [layer, page+padding, page_token, kv_head, head_dim]
            # host   [page, layer, page_token, kv_head, head_dim].
            device_k = torch.empty(
                (plan.layer_count, device_pages + 1, page_tokens, plan.kv_heads_per_rank, plan.head_dim),
                dtype=torch.bfloat16,
                device=f"npu:{device}",
            )
            device_v = torch.empty_like(device_k)
            host_k = torch.empty(
                (max_pages, plan.layer_count, page_tokens, plan.kv_heads_per_rank, plan.head_dim),
                dtype=torch.bfloat16,
                device="cpu",
                pin_memory=True,
            )
            host_v = torch.empty_like(host_k, pin_memory=True)
            layouts.append(
                {
                    "page_tokens": page_tokens,
                    "device_shape": list(device_k.shape),
                    "device_stride": list(device_k.stride()),
                    "host_shape": list(host_k.shape),
                    "host_stride": list(host_k.stride()),
                }
            )
            # Match HiCacheController.move_indices() exactly for the deployed
            # ``kernel_ascend`` backend.  The names describe which KV pool is
            # indexed, not the tensor's residence: both index tensors passed
            # to transfer_kv_dim_exchange are CPU tensors at runtime.
            device_indices = torch.arange(max_pages * page_tokens, dtype=torch.int64, device="cpu")
            host_indices = torch.arange(max_pages * page_tokens, dtype=torch.int64, device="cpu")
            tensors[page_tokens] = (
                device_k,
                device_v,
                host_k,
                host_v,
                device_indices,
                host_indices,
            )
        torch.npu.synchronize()

        def execute(operation: DmaOperation) -> None:
            barrier.wait()
            device_k, device_v, host_k, host_v, device_indices, host_indices = tensors[operation.page_tokens]
            tokens = operation.operation_pages * operation.page_tokens
            transfer_kv_dim_exchange(
                device_indices=device_indices[:tokens],
                host_indices=host_indices[:tokens],
                device_k=device_k,
                host_k=host_k,
                device_v=device_v,
                host_v=host_v,
                page_size=operation.page_tokens,
                direction=TransferDirection.H2D if operation.direction == "host_to_device" else TransferDirection.D2H,
            )
            torch.npu.synchronize()
            barrier.wait()

        seen: set[tuple[str, int, int]] = set()
        for operation in operations:
            key = (operation.direction, operation.page_tokens, operation.operation_pages)
            if key in seen:
                continue
            seen.add(key)
            for _ in range(plan.warmup):
                execute(operation)

        with profiler:
            for operation in operations:
                execute(operation)
        result_queue.put(
            {
                "status": "ok",
                "rank": rank,
                "device": device,
                "cpu_affinity": actual_affinity,
                "numa_preferred_node": actual_numa_node,
                "device_name": str(torch.npu.get_device_name(device)),
                "index_residency": dict(RUNTIME_DMA_INDEX_RESIDENCY),
                "transfer_layouts": layouts,
            }
        )
    except Exception as error:
        try:
            barrier.abort()
        except Exception:
            pass
        result_queue.put({"status": "error", "rank": rank, "error": repr(error)})
        raise


def capture_runtime_dma(plan: RuntimeDmaPlan) -> dict[str, Any]:
    started = time.monotonic()
    report_path = plan.output_dir / "runtime_dma_calibration.json"
    if plan.output_dir.exists():
        raise FileExistsError(f"runtime DMA calibration output already exists: {plan.output_dir}")
    kv_bytes_per_token_per_rank = 2 * plan.layer_count * plan.kv_heads_per_rank * plan.head_dim * plan.element_bytes
    operations = build_operation_plan(
        page_token_sizes=plan.page_token_sizes,
        payload_bytes=plan.payload_bytes,
        kv_bytes_per_token_per_rank=kv_bytes_per_token_per_rank,
        repeats=plan.repeats,
        directions=plan.directions,
    )
    if plan.device_capacity_tokens is not None:
        if any(
            plan.device_capacity_tokens % op.page_tokens
            or plan.device_capacity_tokens < op.operation_pages * op.page_tokens
            for op in operations
        ):
            raise ValueError("Device capacity must contain every operation and be divisible by every page size")

    plan.output_dir.mkdir(parents=True)
    context = multiprocessing.get_context("spawn")
    barrier = context.Barrier(len(plan.devices))
    result_queue = context.Queue()
    processes = []
    try:
        for rank in range(len(plan.devices)):
            process = context.Process(
                target=_profile_rank,
                args=(plan, rank, operations, barrier, result_queue),
                name=f"hicache-runtime-dma-rank{rank}",
            )
            processes.append(process)
            process.start()
        for process in processes:
            process.join(timeout=900.0)
            if process.is_alive():
                raise TimeoutError(f"runtime DMA calibration worker timed out: {process.name}")
        worker_rows = [result_queue.get(timeout=10.0) for _ in processes]
        errors = [row for row in worker_rows if row.get("status") != "ok"]
        bad_exits = [process for process in processes if process.exitcode != 0]
        if errors or bad_exits:
            raise RuntimeError(
                f"runtime DMA calibration worker failure: errors={errors}, "
                f"exitcodes={[process.exitcode for process in processes]}"
            )
    finally:
        for process in processes:
            if process.pid is not None and process.is_alive():
                process.terminate()
                process.join(timeout=30.0)
        result_queue.close()

    worker_by_rank = {int(row["rank"]): row for row in worker_rows}

    samples: list[dict[str, Any]] = []
    operator_tables: list[str] = []
    for rank, _device in enumerate(plan.devices):
        candidates = list((plan.output_dir / "profile_work" / f"rank{rank}").glob("**/operator_details.csv"))
        if len(candidates) != 1:
            raise RuntimeError(f"rank {rank} must emit exactly one operator_details.csv; found {len(candidates)}")
        operator_tables.append(str(candidates[0].relative_to(plan.output_dir)))
        samples.extend(
            parse_operator_table(
                candidates[0],
                operations,
                rank=rank,
            )
        )
    report = {
        "capture_wall_seconds": time.monotonic() - started,
        "operator_tables": operator_tables,
        "environment": {
            "ranks": [worker_by_rank[rank] for rank in sorted(worker_by_rank)],
        },
        "parameters": {
            "devices": plan.devices,
            "directions": list(plan.directions),
            "scope_count": len(plan.devices),
            "cpu_sets": [format_cpu_set(value) for value in plan.cpu_sets],
            "numa_nodes": plan.numa_nodes,
            "page_token_sizes": sorted(plan.page_token_sizes),
            "payload_bytes": sorted(plan.payload_bytes),
            "repeats": plan.repeats,
            "warmup": plan.warmup,
            "profiler_level": plan.profiler_level,
            "device_capacity_tokens": plan.device_capacity_tokens,
            "operator_count_per_rank": len(operations),
            "total_operator_count": len(operations) * len(plan.devices),
            "concurrency": "barrier_aligned_tp_ranks",
        },
        "geometry": {
            "layer_count": plan.layer_count,
            "kv_heads_per_rank": plan.kv_heads_per_rank,
            "head_dim": plan.head_dim,
            "element_bytes": plan.element_bytes,
            "kv_bytes_per_token_per_rank": kv_bytes_per_token_per_rank,
            "device_layout": "[layer,page+padding,page_tokens,kv_head,head_dim]",
            "host_layout": "[page,layer,page_tokens,kv_head,head_dim]",
        },
        "runtime_semantics": {
            "backend": "kernel_ascend",
            "layout": "page_first_direct",
            "operator": RUNTIME_DMA_OPERATOR_NAME,
            "service_clock": "Device Total Duration(us) from operator_details.csv",
            "control_clocks": [
                "Host Self Duration(us) from operator_details.csv",
                "Host Total Duration(us) from operator_details.csv",
            ],
            "host_memory": "pinned CPU; worker NUMA preference does not establish page residency",
            "index_residency": dict(RUNTIME_DMA_INDEX_RESIDENCY),
            "index_semantics_source": (
                "HiCacheController.move_indices(kernel_ascend): return host_indices, device_indices.cpu()"
            ),
        },
        "samples": samples,
        "target_workload_trace_used": False,
        "target_e2e_used": False,
        "model_weights_loaded": False,
    }
    write_json(report_path, report)
    return report


def parse_args(argv: list[str] | None = None) -> RuntimeDmaPlan:
    parser = argparse.ArgumentParser(description="Calibrate the deployed Ascend HiCache DMA primitive.")
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument(
        "--directions",
        nargs="+",
        choices=tuple(DMA_SERVICES),
        default=tuple(DMA_SERVICES),
        help="measure only the missing transfer directions",
    )
    parser.add_argument("--cpu-sets", required=True)
    parser.add_argument(
        "--base-report",
        dest="base_path",
        type=Path,
        required=True,
        help="platform or physical report supplying KV geometry and deployment placement",
    )
    parser.add_argument("--page-token-sizes", default=DEFAULT_PAGE_TOKEN_SIZES)
    parser.add_argument("--payload-bytes", default=DEFAULT_PAYLOAD_BYTES)
    parser.add_argument("--repeats", type=positive_int, default=3)
    parser.add_argument("--warmup", type=nonnegative_int, default=1)
    parser.add_argument(
        "--profiler-level",
        type=int,
        choices=(0, 1, 2),
        default=0,
        help="Keep 0 for calibration; 2 matches SGLang runtime-event coverage diagnostics",
    )
    parser.add_argument(
        "--device-capacity-tokens",
        type=positive_int,
        help="Fix device pool capacity independently of operation sizes; excludes one padding page",
    )
    args = parser.parse_args(argv)
    args.output_dir = require_repo_path(args.output_dir).resolve()
    args.base_path = require_repo_path(args.base_path)
    args.base_report = load_json(args.base_path)
    if (
        args.base_report.get("target_workload_trace_used") is not False
        or args.base_report.get("target_e2e_used") is not False
    ):
        parser.error("DMA supplement requires an independent platform or physical report")

    geometry = args.base_report["kv_geometry"]
    scope = args.base_report["measurement_scope"]
    args.devices = scope["storage_scope_devices"]
    args.numa_nodes = scope["storage_scope_numa_nodes"]
    args.layer_count = positive_int(str(geometry["num_hidden_layers"]))
    args.kv_heads_per_rank = positive_int(str(geometry["num_key_value_heads_per_rank"]))
    args.head_dim = positive_int(str(geometry["head_dim"]))
    args.element_bytes = geometry["kv_element_bytes"]
    if args.element_bytes != 2:
        parser.error("DMA calibration currently supports two-byte KV elements")
    if len(args.devices) != geometry["tensor_parallel_size"] or len(args.numa_nodes) != len(args.devices):
        parser.error("DMA placement must cover the deployment TP ranks")
    if not args.devices or len(set(args.devices)) != len(args.devices) or any(device < 0 for device in args.devices):
        parser.error("DMA devices must be distinct nonnegative indices")
    if any(node is not None and node < 0 for node in args.numa_nodes):
        parser.error("DMA NUMA nodes must be nonnegative or null for inherited policy")

    args.directions = tuple(dict.fromkeys(args.directions))
    args.cpu_sets = parse_cpu_sets(args.cpu_sets, len(args.devices))
    args.page_token_sizes = parse_positive_csv(args.page_token_sizes, "page-token-sizes")
    args.payload_bytes = parse_positive_csv(args.payload_bytes, "payload-bytes")

    return RuntimeDmaPlan(**vars(args))


def main(argv: list[str] | None = None) -> int:
    from .calibration.bundle import complete_dma_bundle

    plan = parse_args(argv)
    report = capture_runtime_dma(plan)
    geometry = plan.base_report["kv_geometry"]["kv_bytes_per_token_per_rank"]
    dma_path = plan.output_dir / "runtime_dma_calibration.json"
    runtime = load_runtime_service_models(
        dma_path,
        expected_kv_bytes_per_token_per_rank=geometry,
        expected_page_bytes=[page * geometry for page in plan.page_token_sizes],
        expected_concurrent_scope_count=len(plan.devices),
        directions=plan.directions,
    )
    complete_dma_bundle(plan.base_report, plan.base_path, dma_path, runtime)
    print(f"runtime_dma_report={plan.output_dir / 'runtime_dma_calibration.json'}")
    print(f"samples={len(report['samples'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
