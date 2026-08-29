"""Shared physical calibration primitives without a persistence contract."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path
from typing import Any

from ..common.io import load_json


def derive_kv_geometry(
    model_config_path: Path,
    tensor_parallel_size: int,
    element_bytes_override: int,
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
    torch_dtype = str(raw.get("torch_dtype") or raw.get("dtype") or "")
    normalized_torch_dtype = torch_dtype.lower().replace("torch.", "")
    element_bytes = element_bytes_override or dtype_bytes(torch_dtype)
    bytes_per_token = layers * 2 * kv_heads_per_rank * head_dim * element_bytes
    return {
        "model_name": path.parent.name.lower().replace("_", "-").replace(" ", "-"),
        "model_config_path": str(path),
        "num_hidden_layers": layers,
        "num_key_value_heads": kv_heads,
        "num_key_value_heads_per_rank": kv_heads_per_rank,
        "num_attention_heads": attention_heads,
        "head_dim": head_dim,
        "kv_torch_dtype": normalized_torch_dtype,
        "kv_element_bytes": element_bytes,
        "tensor_parallel_size": tensor_parallel_size,
        "kv_bytes_per_token_per_rank": bytes_per_token,
        "formula": "layers * 2(K,V) * kv_heads_per_rank * head_dim * element_bytes",
    }


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
    """Map a model KV scalar type to its storage width."""

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


def filesystem_type(path: Path) -> str:
    """Return the filesystem type containing a calibration directory."""

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
