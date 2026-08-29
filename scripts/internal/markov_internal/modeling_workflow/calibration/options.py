"""Command-line boundary for one compact physical calibration."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

from ..physical_calibration import nonnegative_int, positive_int, unit_interval


DEFAULT_PAGE_TOKEN_SIZES = "32,64,128"
DEFAULT_BURST_BYTES_PER_SCOPE = 384 * 1024 * 1024
DEFAULT_EXISTING_OPERATION_PAGES = "1,8,32,64"
DEFAULT_NEW_WRITE_QUEUE_BYTES = "2147483648,2415919104"
STORAGE_PAGE_MATERIALIZATION_RUNTIME = "page_first_direct_mha_runtime"


def parse_integer_csv(value: str, field: str, *, allow_zero: bool = False) -> list[int]:
    try:
        values = [int(item.strip()) for item in value.split(",") if item.strip()]
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"{field} must be comma-separated integers") from error
    minimum = 0 if allow_zero else 1
    if not values or any(item < minimum for item in values) or len(values) != len(set(values)):
        qualifier = "nonnegative" if allow_zero else "positive"
        raise argparse.ArgumentTypeError(f"{field} must contain unique {qualifier} integers")
    return values


def parse_positive_csv(value: str, field: str) -> list[int]:
    return parse_integer_csv(value, field)


def parse_cpu_sets(value: str, scope_count: int, *, optional: bool = False) -> list[set[int] | None]:
    if optional and not value.strip():
        return [None] * scope_count
    groups = [part.strip() for part in value.split("|")]
    if len(groups) != scope_count:
        raise argparse.ArgumentTypeError("storage-scope-cpu-sets needs one set per TP scope")
    output: list[set[int] | None] = []
    for group in groups:
        cpus: set[int] = set()
        for item in filter(None, (part.strip() for part in group.split(","))):
            if "-" in item:
                start, end = (int(value) for value in item.split("-", 1))
                if start < 0 or end < start:
                    raise argparse.ArgumentTypeError(f"invalid CPU range: {item}")
                cpus.update(range(start, end + 1))
            else:
                cpu = int(item)
                if cpu < 0:
                    raise argparse.ArgumentTypeError(f"invalid CPU index: {item}")
                cpus.add(cpu)
        if not cpus:
            raise argparse.ArgumentTypeError("storage CPU sets must not be empty")
        output.append(cpus)
    return output


def format_cpu_set(value: set[int]) -> str:
    return ",".join(str(cpu) for cpu in sorted(value))


@dataclass(frozen=True)
class CalibrationOptions:
    output_dir: Path
    model_config: Path
    tensor_parallel_size: int
    storage_dir: Path
    runtime_dma_report: Path
    concurrent_runtime_dma_report: Path
    control_primitives: Path
    page_token_sizes: str
    burst_bytes_per_scope: int
    warmup: int
    repeats: int
    isolated_repeats: int
    storage_existing_operation_pages: str
    storage_new_write_queue_bytes_per_scope: str
    storage_new_write_operation_bytes_per_scope: str
    selection_percentile: float
    kv_element_bytes: int
    storage_scope_cpu_sets: str
    force: bool

def parse_args(argv: list[str] | None = None) -> CalibrationOptions:
    parser = argparse.ArgumentParser(description="Calibrate compact HiCache physical I/O primitives.")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--model-config", type=Path, required=True)
    parser.add_argument("--tensor-parallel-size", type=positive_int, required=True)
    parser.add_argument("--storage-dir", type=Path, required=True)
    parser.add_argument("--runtime-dma-report", type=Path, required=True)
    parser.add_argument("--concurrent-runtime-dma-report", type=Path, required=True)
    parser.add_argument(
        "--control-primitives",
        type=Path,
        required=True,
        help="Snapshot-free control calibration containing zero-payload prefetch and load per-page scalars.",
    )
    parser.add_argument("--page-token-sizes", default=DEFAULT_PAGE_TOKEN_SIZES)
    parser.add_argument("--burst-bytes-per-scope", type=positive_int, default=DEFAULT_BURST_BYTES_PER_SCOPE)
    parser.add_argument("--warmup", type=nonnegative_int, default=1)
    parser.add_argument("--repeats", type=positive_int, default=5)
    parser.add_argument("--isolated-repeats", type=positive_int, default=5)
    parser.add_argument("--storage-existing-operation-pages", default=DEFAULT_EXISTING_OPERATION_PAGES)
    parser.add_argument("--storage-new-write-queue-bytes-per-scope", default=DEFAULT_NEW_WRITE_QUEUE_BYTES)
    parser.add_argument("--storage-new-write-operation-bytes-per-scope", default="")
    parser.add_argument("--selection-percentile", type=unit_interval, default=0.25)
    parser.add_argument("--kv-element-bytes", type=nonnegative_int, default=0)
    parser.add_argument("--storage-scope-cpu-sets", default="")
    parser.add_argument("--force", action="store_true")
    return CalibrationOptions(**vars(parser.parse_args(argv)))
