"""Compact calibration for the actual Ascend HiCache DMA primitive.

The generic ``Tensor.copy_`` clock does not match SGLang's deployed
``kernel_ascend`` path.  This module executes ``transfer_kv_dim_exchange``
with the Qwen MHA page-first layouts, profiles the runtime operator, and keeps
only normalized samples plus the small operator table needed for audit.

No model weights, request workload, target profile, or target latency label is
opened by this calibration.
"""

from __future__ import annotations

import argparse
import csv
import math
import multiprocessing
import os
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..common.io import write_json
from ..common.paths import require_repo_path
from .calibration.runtime_anchors import (
    RUNTIME_DMA_INDEX_RESIDENCY,
    RUNTIME_DMA_OPERATOR_NAME,
)
from .calibration.options import format_cpu_set, parse_cpu_sets, parse_integer_csv, parse_positive_csv
from .physical_calibration import nonnegative_int, positive_int

DEFAULT_PAGE_TOKEN_SIZES = "32,64,128"
DEFAULT_PAYLOAD_BYTES = "134217728,251658240,385875968"
DEFAULT_LAYER_COUNT = 64
DEFAULT_KV_HEADS_PER_RANK = 4
DEFAULT_HEAD_DIM = 128
DEFAULT_ELEMENT_BYTES = 2


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
) -> list[DmaOperation]:
    """Build the deterministic operator order used to decode profiler rows."""

    if repeats <= 0 or kv_bytes_per_token_per_rank <= 0:
        raise ValueError("runtime DMA plan requires positive repeats and KV geometry")
    operations: list[DmaOperation] = []
    sample_index = 0
    for page_tokens in sorted(page_token_sizes):
        page_bytes = page_tokens * kv_bytes_per_token_per_rank
        operation_pages = [1]
        for payload in sorted(payload_bytes):
            if payload % page_bytes:
                raise ValueError(f"payload bytes {payload} is not divisible by page bytes {page_bytes}")
            pages = payload // page_bytes
            if pages not in operation_pages:
                operation_pages.append(pages)
        for direction in ("host_to_device", "device_to_host"):
            for pages in operation_pages:
                for ordinal in range(repeats):
                    operations.append(
                        DmaOperation(
                            sample_index=sample_index,
                            direction=direction,
                            page_tokens=page_tokens,
                            page_bytes=page_bytes,
                            operation_pages=pages,
                            byte_count=pages * page_bytes,
                            ordinal=ordinal,
                        )
                    )
                    sample_index += 1
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
                "direction": operation.direction,
                "page_bytes": operation.page_bytes,
                "operation_pages": operation.operation_pages,
                "bytes": operation.byte_count,
                "device_duration_us": duration_us,
                "bandwidth_bytes_per_sec": operation.byte_count * 1_000_000.0 / duration_us,
            }
        )
    return samples


def _execute_transfer(
    *,
    operation: DmaOperation,
    device_k: Any,
    device_v: Any,
    host_k: Any,
    host_v: Any,
    device_indices: Any,
    host_indices: Any,
    transfer: Any,
    transfer_direction: Any,
) -> None:
    transfer(
        device_indices=device_indices[: operation.operation_pages * operation.page_tokens],
        host_indices=host_indices[: operation.operation_pages * operation.page_tokens],
        device_k=device_k,
        host_k=host_k,
        device_v=device_v,
        host_v=host_v,
        page_size=operation.page_tokens,
        direction=(transfer_direction.H2D if operation.direction == "host_to_device" else transfer_direction.D2H),
    )


def _profile_rank(
    *,
    rank: int,
    device: int,
    cpu_set: set[int],
    operations: list[DmaOperation],
    warmup: int,
    layer_count: int,
    kv_heads_per_rank: int,
    head_dim: int,
    output_dir: Path,
    barrier: Any,
    result_queue: Any,
) -> None:
    try:
        os.sched_setaffinity(0, cpu_set)
        actual_affinity = sorted(os.sched_getaffinity(0))
        if set(actual_affinity) != cpu_set:
            raise RuntimeError(
                f"rank {rank} CPU affinity mismatch: requested={sorted(cpu_set)}, actual={actual_affinity}"
            )
        import torch
        import torch_npu
        from sgl_kernel_npu.kvcacheio import TransferDirection, transfer_kv_dim_exchange

        torch.npu.set_device(device)
        profile_root = output_dir / "profile_work" / f"rank{rank}"
        profile_root.mkdir(parents=True, exist_ok=True)
        experimental = torch_npu.profiler._ExperimentalConfig(
            export_type=[torch_npu.profiler.ExportType.Text],
            profiler_level=torch_npu.profiler.ProfilerLevel.Level0,
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
        for page_tokens, group in groups.items():
            max_pages = max(operation.operation_pages for operation in group)
            # These are the deployed NPU MHA layouts:
            # device [layer, page+padding, page_token, kv_head, head_dim]
            # host   [page, layer, page_token, kv_head, head_dim].
            device_k = torch.empty(
                (layer_count, max_pages + 1, page_tokens, kv_heads_per_rank, head_dim),
                dtype=torch.bfloat16,
                device=f"npu:{device}",
            )
            device_v = torch.empty_like(device_k)
            host_k = torch.empty(
                (max_pages, layer_count, page_tokens, kv_heads_per_rank, head_dim),
                dtype=torch.bfloat16,
                pin_memory=True,
            )
            host_v = torch.empty_like(host_k, pin_memory=True)
            # Match HiCacheController.move_indices() exactly for the deployed
            # ``kernel_ascend`` backend.  The names describe which KV pool is
            # indexed, not the tensor's residence: both index tensors passed
            # to transfer_kv_dim_exchange are CPU tensors at runtime.
            device_indices = torch.arange(max_pages * page_tokens, dtype=torch.int64)
            host_indices = torch.arange(max_pages * page_tokens, dtype=torch.int64)
            if device_indices.device.type != "cpu" or host_indices.device.type != "cpu":
                raise RuntimeError("kernel_ascend calibration indices must both reside on CPU")
            tensors[page_tokens] = (
                device_k,
                device_v,
                host_k,
                host_v,
                device_indices,
                host_indices,
            )
        torch.npu.synchronize()

        warmup_operations: list[DmaOperation] = []
        seen: set[tuple[str, int, int]] = set()
        for operation in operations:
            key = (operation.direction, operation.page_tokens, operation.operation_pages)
            if key in seen:
                continue
            seen.add(key)
            warmup_operations.extend([operation] * warmup)
        for operation in warmup_operations:
            barrier.wait()
            _execute_transfer(
                operation=operation,
                device_k=tensors[operation.page_tokens][0],
                device_v=tensors[operation.page_tokens][1],
                host_k=tensors[operation.page_tokens][2],
                host_v=tensors[operation.page_tokens][3],
                device_indices=tensors[operation.page_tokens][4],
                host_indices=tensors[operation.page_tokens][5],
                transfer=transfer_kv_dim_exchange,
                transfer_direction=TransferDirection,
            )
            torch.npu.synchronize()
            barrier.wait()

        with profiler:
            for operation in operations:
                barrier.wait()
                _execute_transfer(
                    operation=operation,
                    device_k=tensors[operation.page_tokens][0],
                    device_v=tensors[operation.page_tokens][1],
                    host_k=tensors[operation.page_tokens][2],
                    host_v=tensors[operation.page_tokens][3],
                    device_indices=tensors[operation.page_tokens][4],
                    host_indices=tensors[operation.page_tokens][5],
                    transfer=transfer_kv_dim_exchange,
                    transfer_direction=TransferDirection,
                )
                torch.npu.synchronize()
                barrier.wait()
        result_queue.put(
            {
                "status": "ok",
                "rank": rank,
                "device": device,
                "cpu_affinity": actual_affinity,
                "device_name": str(torch.npu.get_device_name(device)),
                "index_residency": dict(RUNTIME_DMA_INDEX_RESIDENCY),
            }
        )
    except Exception as error:
        try:
            barrier.abort()
        except Exception:
            pass
        result_queue.put({"status": "error", "rank": rank, "error": repr(error)})
        raise


def capture_runtime_dma(
    *,
    output_dir: Path,
    devices: list[int],
    cpu_sets: list[set[int]],
    page_token_sizes: list[int],
    payload_bytes: list[int],
    repeats: int,
    warmup: int,
    layer_count: int,
    kv_heads_per_rank: int,
    head_dim: int,
    element_bytes: int,
) -> dict[str, Any]:
    output_dir = require_repo_path(output_dir).resolve()
    report_path = output_dir / "runtime_dma_calibration.json"
    if output_dir.exists():
        raise FileExistsError(f"runtime DMA calibration output already exists: {output_dir}")
    output_dir.mkdir(parents=True)
    kv_bytes_per_token_per_rank = 2 * layer_count * kv_heads_per_rank * head_dim * element_bytes
    operations = build_operation_plan(
        page_token_sizes=page_token_sizes,
        payload_bytes=payload_bytes,
        kv_bytes_per_token_per_rank=kv_bytes_per_token_per_rank,
        repeats=repeats,
    )
    context = multiprocessing.get_context("spawn")
    barrier = context.Barrier(len(devices))
    result_queue = context.Queue()
    processes = []
    for rank, (device, cpu_set) in enumerate(zip(devices, cpu_sets)):
        process = context.Process(
            target=_profile_rank,
            kwargs={
                "rank": rank,
                "device": device,
                "cpu_set": cpu_set,
                "operations": operations,
                "warmup": warmup,
                "layer_count": layer_count,
                "kv_heads_per_rank": kv_heads_per_rank,
                "head_dim": head_dim,
                "output_dir": output_dir,
                "barrier": barrier,
                "result_queue": result_queue,
            },
            name=f"hicache-runtime-dma-rank{rank}",
        )
        process.start()
        processes.append(process)
    for process in processes:
        process.join(timeout=900.0)
    for process in processes:
        if process.is_alive():
            process.terminate()
            process.join(timeout=30.0)
            raise TimeoutError(f"runtime DMA calibration worker timed out: {process.name}")
    worker_rows = [result_queue.get(timeout=10.0) for _ in processes]
    errors = [row for row in worker_rows if row.get("status") != "ok"]
    bad_exits = [process for process in processes if process.exitcode != 0]
    if errors or bad_exits:
        raise RuntimeError(
            f"runtime DMA calibration worker failure: errors={errors}, "
            f"exitcodes={[process.exitcode for process in processes]}"
        )
    worker_by_rank = {int(row["rank"]): row for row in worker_rows}

    samples: list[dict[str, Any]] = []
    for rank, _device in enumerate(devices):
        candidates = list((output_dir / "profile_work" / f"rank{rank}").glob("**/operator_details.csv"))
        if len(candidates) != 1:
            raise RuntimeError(f"rank {rank} must emit exactly one operator_details.csv; found {len(candidates)}")
        samples.extend(
            parse_operator_table(
                candidates[0],
                operations,
                rank=rank,
            )
        )
    shutil.rmtree(output_dir / "profile_work")

    report = {
        "environment": {
            "ranks": [worker_by_rank[rank] for rank in sorted(worker_by_rank)],
        },
        "parameters": {
            "devices": devices,
            "scope_count": len(devices),
            "cpu_sets": [format_cpu_set(value) for value in cpu_sets],
            "page_token_sizes": sorted(page_token_sizes),
            "payload_bytes": sorted(payload_bytes),
            "repeats": repeats,
            "warmup": warmup,
            "operator_count_per_rank": len(operations),
            "total_operator_count": len(operations) * len(devices),
            "concurrency": "barrier_aligned_tp_ranks",
        },
        "geometry": {
            "layer_count": layer_count,
            "kv_heads_per_rank": kv_heads_per_rank,
            "head_dim": head_dim,
            "element_bytes": element_bytes,
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
            "host_memory": "NUMA-local pinned CPU",
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


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Calibrate the deployed Ascend HiCache DMA primitive.")
    capture = parser
    capture.add_argument("--output-dir", required=True, type=Path)
    capture.add_argument("--devices", default="0,1")
    capture.add_argument("--cpu-sets", required=True)
    capture.add_argument("--page-token-sizes", default=DEFAULT_PAGE_TOKEN_SIZES)
    capture.add_argument("--payload-bytes", default=DEFAULT_PAYLOAD_BYTES)
    capture.add_argument("--repeats", type=positive_int, default=3)
    capture.add_argument("--warmup", type=nonnegative_int, default=1)
    capture.add_argument("--layer-count", type=positive_int, default=DEFAULT_LAYER_COUNT)
    capture.add_argument("--kv-heads-per-rank", type=positive_int, default=DEFAULT_KV_HEADS_PER_RANK)
    capture.add_argument("--head-dim", type=positive_int, default=DEFAULT_HEAD_DIM)
    capture.add_argument("--element-bytes", type=positive_int, default=DEFAULT_ELEMENT_BYTES)

    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    devices = parse_integer_csv(args.devices, "devices", allow_zero=True)
    cpu_sets = parse_cpu_sets(args.cpu_sets, len(devices))
    report = capture_runtime_dma(
        output_dir=args.output_dir,
        devices=devices,
        cpu_sets=[value for value in cpu_sets if value is not None],
        page_token_sizes=parse_positive_csv(args.page_token_sizes, "page-token-sizes"),
        payload_bytes=parse_positive_csv(args.payload_bytes, "payload-bytes"),
        repeats=args.repeats,
        warmup=args.warmup,
        layer_count=args.layer_count,
        kv_heads_per_rank=args.kv_heads_per_rank,
        head_dim=args.head_dim,
        element_bytes=args.element_bytes,
    )
    print(f"runtime_dma_report={require_repo_path(args.output_dir).resolve() / 'runtime_dma_calibration.json'}")
    print(f"samples={len(report['samples'])}")
    print("model_workload_profiles_run=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
