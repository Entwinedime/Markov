"""Independent HiCache model-geometry and I/O bandwidth calibration."""

from __future__ import annotations

import argparse
import os
import platform
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

from ..common.digests import sha256_file
from ..common.io import load_json, write_json
from ..common.paths import repo_relative_path, require_repo_path
from .io_model import HICACHE_IO_MODEL_SCHEMA, HiCacheIoModel


CALIBRATION_REPORT_SCHEMA = "markov.hicache.io_calibration_report.v1"
DEFAULT_TRANSFER_BYTES = 128 * 1024 * 1024
DEFAULT_WARMUP = 2
DEFAULT_REPEATS = 7
DEFAULT_PERCENTILE = 0.25


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse one explicit calibration run."""

    parser = argparse.ArgumentParser(description="Calibrate the two-parameter HiCache I/O model.")
    parser.add_argument(
        "--output-dir", type=Path, required=True, help="Repository directory for the model and raw report."
    )
    parser.add_argument(
        "--model-config", type=Path, required=True, help="Hugging Face model config.json path visible in the container."
    )
    parser.add_argument(
        "--tensor-parallel-size",
        type=positive_int,
        required=True,
        help="Tensor-parallel size used by the target deployment.",
    )
    parser.add_argument("--devices", default="0", help="Comma-separated NPU device indices to sample.")
    parser.add_argument(
        "--storage-dir", type=Path, required=True, help="Repository directory on the HiCache file-backend filesystem."
    )
    parser.add_argument(
        "--transfer-bytes",
        type=positive_int,
        default=DEFAULT_TRANSFER_BYTES,
        help="Bytes copied by every timing sample.",
    )
    parser.add_argument(
        "--warmup", type=nonnegative_int, default=DEFAULT_WARMUP, help="Untimed warmup repetitions per direction."
    )
    parser.add_argument(
        "--repeats", type=positive_int, default=DEFAULT_REPEATS, help="Timed repetitions per direction."
    )
    parser.add_argument(
        "--selection-percentile",
        type=unit_interval,
        default=DEFAULT_PERCENTILE,
        help="Per-direction throughput percentile used before taking the conservative minimum.",
    )
    parser.add_argument(
        "--kv-element-bytes",
        type=nonnegative_int,
        default=0,
        help="Override KV scalar bytes; 0 derives from torch_dtype.",
    )
    parser.add_argument("--force", action="store_true", help="Replace an existing calibration report and model.")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """Run calibration and write one strict model plus its complete provenance."""

    args = parse_args(argv)
    output_dir = require_repo_path(args.output_dir).resolve()
    storage_dir = require_repo_path(args.storage_dir).resolve()
    model_path = output_dir / "hicache_io_model.json"
    report_path = output_dir / "calibration_report.json"
    if not args.force and (model_path.exists() or report_path.exists()):
        raise FileExistsError(f"calibration output already exists: {output_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)
    storage_dir.mkdir(parents=True, exist_ok=True)

    devices = parse_devices(args.devices)
    geometry = derive_kv_geometry(args.model_config, args.tensor_parallel_size, args.kv_element_bytes)
    device_samples, device_environment = calibrate_device_host(
        devices=devices,
        transfer_bytes=args.transfer_bytes,
        warmup=args.warmup,
        repeats=args.repeats,
    )
    storage_samples = calibrate_file_backend(
        storage_dir=storage_dir,
        transfer_bytes=args.transfer_bytes,
        warmup=args.warmup,
        repeats=args.repeats,
    )

    device_summary = summarize_grouped_samples(device_samples, args.selection_percentile)
    storage_summary = summarize_grouped_samples(storage_samples, args.selection_percentile)
    device_bandwidth = conservative_bandwidth(device_summary)
    storage_bandwidth = conservative_bandwidth(storage_summary)
    created_at = datetime.now(timezone.utc).isoformat()
    model_id = f"hicache-io-{geometry['model_name']}-tp{args.tensor_parallel_size}-{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')}"

    report = {
        "schema": CALIBRATION_REPORT_SCHEMA,
        "created_at": created_at,
        "model_id": model_id,
        "command": [sys.executable, "-m", "markov_internal.modeling_workflow.io_calibration", *sys.argv[1:]],
        "repository_commit": repository_commit(),
        "environment": {
            "python": sys.version,
            "platform": platform.platform(),
            "device": device_environment,
            "storage": {
                "path": str(repo_relative_path(storage_dir)),
                "filesystem": filesystem_type(storage_dir),
                "backend_semantics": "numpy.tofile write; fdatasync+POSIX_FADV_DONTNEED after timing; unbuffered readinto",
            },
        },
        "parameters": {
            "transfer_bytes": args.transfer_bytes,
            "warmup": args.warmup,
            "repeats": args.repeats,
            "selection_percentile": args.selection_percentile,
            "devices": devices,
            "tensor_parallel_size": args.tensor_parallel_size,
        },
        "kv_geometry": geometry,
        "device_host": {
            "samples": device_samples,
            "summary": device_summary,
            "selected_bandwidth_bytes_per_sec": device_bandwidth,
            "selection_rule": "minimum selected percentile across every sampled device and H2D/D2H direction",
        },
        "host_storage": {
            "samples": storage_samples,
            "summary": storage_summary,
            "selected_bandwidth_bytes_per_sec": storage_bandwidth,
            "selection_rule": "minimum selected percentile across HiCacheFile-compatible read/write directions",
        },
        "target_workload_trace_used": False,
        "target_e2e_used": False,
    }
    write_json(report_path, report)

    model = {
        "schema": HICACHE_IO_MODEL_SCHEMA,
        "model_id": model_id,
        "calibration_status": "calibrated",
        "kv_bytes_per_token_per_rank": geometry["kv_bytes_per_token_per_rank"],
        "device_host_bandwidth_bytes_per_sec": device_bandwidth,
        "host_storage_bandwidth_bytes_per_sec": storage_bandwidth,
        "provenance": {
            "kv_geometry": (
                f"model_config={geometry['model_config_sha256']};layers={geometry['num_hidden_layers']};"
                f"kv_heads_per_rank={geometry['num_key_value_heads_per_rank']};head_dim={geometry['head_dim']};"
                f"element_bytes={geometry['kv_element_bytes']}"
            ),
            "device_host_bandwidth": (
                f"report={repo_relative_path(report_path)};selection=min_p{args.selection_percentile:g}_across_devices_and_directions;"
                f"bytes={args.transfer_bytes};repeats={args.repeats}"
            ),
            "host_storage_bandwidth": (
                f"report={repo_relative_path(report_path)};backend=hicache_file_buffered;"
                f"selection=min_p{args.selection_percentile:g}_read_write;bytes={args.transfer_bytes};repeats={args.repeats}"
            ),
        },
    }
    write_json(model_path, model)
    validated = HiCacheIoModel.load(model_path)
    print(f"calibration_report={report_path}")
    print(f"hicache_io_model={model_path}")
    print(f"model_digest={validated.digest}")
    print(f"kv_bytes_per_token_per_rank={validated.kv_bytes_per_token_per_rank}")
    print(f"device_host_bandwidth_bytes_per_sec={validated.device_host_bandwidth_bytes_per_sec}")
    print(f"host_storage_bandwidth_bytes_per_sec={validated.host_storage_bandwidth_bytes_per_sec}")
    return 0


def derive_kv_geometry(
    model_config_path: Path, tensor_parallel_size: int, element_bytes_override: int
) -> dict[str, Any]:
    """Derive per-rank KV bytes from the deployment model configuration."""

    path = model_config_path.expanduser().resolve()
    raw = load_json(path)
    if not isinstance(raw, dict):
        raise ValueError(f"model config must be an object: {path}")
    layers = required_positive_int(raw.get("num_hidden_layers"), "num_hidden_layers")
    kv_heads = required_positive_int(raw.get("num_key_value_heads"), "num_key_value_heads")
    attention_heads = required_positive_int(raw.get("num_attention_heads"), "num_attention_heads")
    hidden_size = required_positive_int(raw.get("hidden_size"), "hidden_size")
    head_dim = required_positive_int(raw.get("head_dim") or hidden_size // attention_heads, "head_dim")
    if kv_heads % tensor_parallel_size != 0:
        raise ValueError("num_key_value_heads must be divisible by tensor_parallel_size for strict per-rank geometry")
    kv_heads_per_rank = kv_heads // tensor_parallel_size
    element_bytes = element_bytes_override or dtype_bytes(str(raw.get("torch_dtype") or raw.get("dtype") or ""))
    bytes_per_token = layers * 2 * kv_heads_per_rank * head_dim * element_bytes
    return {
        "model_name": path.parent.name.lower().replace("_", "-").replace(" ", "-"),
        "model_config_path": str(path),
        "model_config_sha256": sha256_file(path),
        "num_hidden_layers": layers,
        "num_key_value_heads": kv_heads,
        "num_key_value_heads_per_rank": kv_heads_per_rank,
        "num_attention_heads": attention_heads,
        "head_dim": head_dim,
        "kv_element_bytes": element_bytes,
        "tensor_parallel_size": tensor_parallel_size,
        "kv_bytes_per_token_per_rank": bytes_per_token,
        "formula": "layers * 2(K,V) * kv_heads_per_rank * head_dim * element_bytes",
    }


def calibrate_device_host(
    *, devices: list[int], transfer_bytes: int, warmup: int, repeats: int
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Measure pinned-host H2D and D2H copies with explicit NPU synchronization."""

    import torch
    import torch_npu

    if transfer_bytes % 4 != 0:
        raise ValueError("transfer_bytes must be divisible by four for float32 copy calibration")
    samples: list[dict[str, Any]] = []
    device_names: dict[str, str] = {}
    element_count = transfer_bytes // 4
    for device_index in devices:
        torch.npu.set_device(device_index)
        device_names[str(device_index)] = str(torch.npu.get_device_name(device_index))
        host = torch.empty(element_count, dtype=torch.float32, pin_memory=True)
        device = torch.empty(element_count, dtype=torch.float32, device=f"npu:{device_index}")
        host.fill_(1.0)
        device.fill_(2.0)
        torch.npu.synchronize()
        for _ in range(warmup):
            device.copy_(host, non_blocking=True)
            torch.npu.synchronize()
            host.copy_(device, non_blocking=True)
            torch.npu.synchronize()
        for direction, copy in (
            ("host_to_device", lambda: device.copy_(host, non_blocking=True)),
            ("device_to_host", lambda: host.copy_(device, non_blocking=True)),
        ):
            for ordinal in range(repeats):
                duration_ns = timed_ns(copy, torch.npu.synchronize)
                samples.append(sample_row(f"device:{device_index}/{direction}", ordinal, transfer_bytes, duration_ns))
        del device
        del host
        torch.npu.empty_cache()
    return samples, {
        "torch_version": str(torch.__version__),
        "torch_npu_version": str(torch_npu.__version__),
        "device_count": int(torch.npu.device_count()),
        "sampled_device_names": device_names,
        "copy_semantics": "pinned CPU float32 tensor; non_blocking Tensor.copy_; torch.npu.synchronize around every sample",
    }


def calibrate_file_backend(
    *, storage_dir: Path, transfer_bytes: int, warmup: int, repeats: int
) -> list[dict[str, Any]]:
    """Measure the buffered APIs used by SGLang's HiCacheFile backend."""

    import numpy as np

    source = np.empty(transfer_bytes, dtype=np.uint8)
    source.fill(0x5A)
    target = np.empty_like(source)
    samples: list[dict[str, Any]] = []
    for ordinal in range(warmup + repeats):
        path = storage_dir / f"hicache_io_calibration_{os.getpid()}_{ordinal}.bin"
        try:
            start = time.perf_counter_ns()
            source.tofile(path)
            write_duration = time.perf_counter_ns() - start
            flush_and_evict(path)

            start = time.perf_counter_ns()
            read_exact_into(path, target)
            read_duration = time.perf_counter_ns() - start
            if ordinal >= warmup:
                sample_ordinal = ordinal - warmup
                samples.append(sample_row("host_storage/write", sample_ordinal, transfer_bytes, write_duration))
                samples.append(sample_row("host_storage/read", sample_ordinal, transfer_bytes, read_duration))
        finally:
            path.unlink(missing_ok=True)
    return samples


def timed_ns(operation: Callable[[], Any], synchronize: Callable[[], Any]) -> int:
    """Measure one asynchronous device operation including completion."""

    synchronize()
    start = time.perf_counter_ns()
    operation()
    synchronize()
    return time.perf_counter_ns() - start


def flush_and_evict(path: Path) -> None:
    """Flush dirty pages and request cache eviction before the read sample."""

    descriptor = os.open(path, os.O_RDWR)
    try:
        os.fdatasync(descriptor)
        if hasattr(os, "posix_fadvise") and hasattr(os, "POSIX_FADV_DONTNEED"):
            os.posix_fadvise(descriptor, 0, 0, os.POSIX_FADV_DONTNEED)
    finally:
        os.close(descriptor)


def read_exact_into(path: Path, target: Any) -> None:
    """Use the same unbuffered readinto pattern as HiCacheFile.get()."""

    view = memoryview(target)
    offset = 0
    with path.open("rb", buffering=0) as file_obj:
        while offset < len(view):
            read = file_obj.readinto(view[offset:])
            if not read:
                break
            offset += read
    if offset != len(view):
        raise OSError(f"short calibration read: expected={len(view)} actual={offset}")


def sample_row(group: str, ordinal: int, byte_count: int, duration_ns: int) -> dict[str, Any]:
    """Build one raw throughput sample."""

    if duration_ns <= 0:
        raise ValueError(f"non-positive calibration duration for {group}")
    return {
        "group": group,
        "ordinal": ordinal,
        "bytes": byte_count,
        "duration_ns": duration_ns,
        "bandwidth_bytes_per_sec": int(byte_count * 1_000_000_000 // duration_ns),
    }


def summarize_grouped_samples(samples: list[dict[str, Any]], selected_percentile: float) -> dict[str, Any]:
    """Summarize bandwidth samples independently for every resource direction."""

    grouped: dict[str, list[int]] = {}
    for sample in samples:
        grouped.setdefault(str(sample["group"]), []).append(int(sample["bandwidth_bytes_per_sec"]))
    return {
        group: {
            "count": len(values),
            "minimum": min(values),
            "selected_percentile": percentile(values, selected_percentile),
            "median": percentile(values, 0.5),
            "mean": int(statistics.fmean(values)),
            "maximum": max(values),
        }
        for group, values in sorted(grouped.items())
    }


def conservative_bandwidth(summary: dict[str, Any]) -> int:
    """Select the lowest configured percentile across all measured directions."""

    values = [int(row["selected_percentile"]) for row in summary.values()]
    if not values or min(values) <= 0:
        raise ValueError("calibration produced no positive bandwidth")
    return min(values)


def percentile(values: list[int], probability: float) -> int:
    """Return a deterministic linearly interpolated percentile."""

    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = probability * (len(ordered) - 1)
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    fraction = position - lower
    return int(ordered[lower] + (ordered[upper] - ordered[lower]) * fraction)


def dtype_bytes(dtype: str) -> int:
    """Map the model KV scalar type to its storage width."""

    normalized = dtype.lower().replace("torch.", "")
    widths = {
        "float16": 2,
        "half": 2,
        "bfloat16": 2,
        "float32": 4,
        "float": 4,
        "int8": 1,
        "uint8": 1,
    }
    if normalized not in widths:
        raise ValueError(f"unsupported model torch_dtype for KV geometry: {dtype!r}")
    return widths[normalized]


def parse_devices(value: str) -> list[int]:
    """Parse unique non-negative NPU indices."""

    devices: list[int] = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        device = nonnegative_int(item)
        if device not in devices:
            devices.append(device)
    if not devices:
        raise ValueError("at least one NPU device is required")
    return devices


def repository_commit() -> str:
    """Return the source revision recorded with the calibration environment."""

    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    return result.stdout.strip() if result.returncode == 0 else "unknown"


def filesystem_type(path: Path) -> str:
    """Return the filesystem type containing the calibration directory."""

    result = subprocess.run(
        ["stat", "-f", "-c", "%T", str(path)],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    return result.stdout.strip() if result.returncode == 0 else "unknown"


def required_positive_int(value: Any, field: str) -> int:
    """Validate one positive integer from model metadata."""

    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(f"model config field {field!r} must be a positive integer")
    return value


def positive_int(value: str) -> int:
    """Parse one positive CLI integer."""

    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("expected a positive integer")
    return parsed


def nonnegative_int(value: str) -> int:
    """Parse one non-negative CLI integer."""

    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("expected a non-negative integer")
    return parsed


def unit_interval(value: str) -> float:
    """Parse one closed-unit-interval CLI value."""

    parsed = float(value)
    if not 0.0 <= parsed <= 1.0:
        raise argparse.ArgumentTypeError("expected a value between zero and one")
    return parsed


if __name__ == "__main__":
    raise SystemExit(main())
