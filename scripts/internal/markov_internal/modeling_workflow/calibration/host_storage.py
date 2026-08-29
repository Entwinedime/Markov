"""Process-isolated host-storage calibration sampling."""

from __future__ import annotations

import math
import multiprocessing
import os
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .options import STORAGE_PAGE_MATERIALIZATION_RUNTIME, parse_positive_csv
from ..physical_calibration import sample_row


STORAGE_SOURCE_WORKING_SET_RUNTIME = "operation_payload_distinct_page_sources"


@dataclass(frozen=True)
class HostStorageCapturePlan:
    """Validated physical grid consumed by the storage sampler."""

    storage_dir: Path
    page_sizes: tuple[int, ...]
    scope_count: int
    model_name: str
    warmup: int
    repeats: int
    isolated_repeats: int
    scope_cpu_sets: tuple[frozenset[int] | None, ...]
    existing_operation_pages: tuple[int, ...]
    new_write_queues: tuple[int, ...]
    new_write_operations: tuple[int, ...]


@dataclass(frozen=True)
class HostStorageCapture:
    """Raw physical samples and worker environments from one sampling pass."""

    storage_samples: tuple[dict[str, Any], ...]
    new_write_samples: tuple[dict[str, Any], ...]


def capture_host_storage(plan: HostStorageCapturePlan) -> HostStorageCapture:
    """Execute the complete process-only host-storage sampling grid."""

    scope_cpu_sets = [set(value) if value is not None else None for value in plan.scope_cpu_sets]
    storage_samples = calibrate_storage_curves(
        storage_dir=plan.storage_dir,
        page_sizes=list(plan.page_sizes),
        scope_count=plan.scope_count,
        model_name=plan.model_name,
        warmup=plan.warmup,
        repeats=plan.repeats,
        isolated_repeats=plan.isolated_repeats,
        scope_cpu_sets=scope_cpu_sets,
        existing_operation_pages=list(plan.existing_operation_pages),
    )
    new_write_samples = calibrate_new_write_only(
        storage_dir=plan.storage_dir,
        page_sizes=list(plan.page_sizes),
        queue_bytes_per_scope=list(plan.new_write_queues),
        operation_bytes_per_scope=list(plan.new_write_operations),
        scope_count=plan.scope_count,
        model_name=plan.model_name,
        warmup=plan.warmup,
        repeats=plan.repeats,
        scope_cpu_sets=scope_cpu_sets,
    )
    return HostStorageCapture(
        storage_samples=tuple(storage_samples),
        new_write_samples=tuple(new_write_samples),
    )


class StorageScopeProcesses:
    """Persistent one-process-per-scope executor for storage calibration.

    SGLang deploys TP ranks as separate processes.  A thread pool in one Python
    process introduces a shared interpreter and does not reproduce that resource
    state, so the default calibration path mirrors the rank process boundary.
    """

    _RESULT_TIMEOUT_SEC = 600.0

    def __init__(
        self,
        *,
        storage_dir: Path,
        scope_count: int,
        model_name: str,
        scope_cpu_sets: list[set[int] | None] | None = None,
    ) -> None:
        if scope_count <= 0:
            raise ValueError("storage scope process count must be positive")
        if scope_cpu_sets is None:
            scope_cpu_sets = [None] * scope_count
        if len(scope_cpu_sets) != scope_count:
            raise ValueError("storage scope CPU set count must match scope count")
        context = multiprocessing.get_context("spawn")
        self._processes: list[multiprocessing.Process] = []
        self._commands: list[Any] = []
        self._results: list[Any] = []
        self._next_task_id = 1
        for scope in range(scope_count):
            command = context.Queue()
            result = context.Queue()
            process = context.Process(
                target=_storage_scope_process_main,
                args=(
                    str(storage_dir),
                    scope,
                    scope_count,
                    model_name,
                    tuple(sorted(scope_cpu_sets[scope])) if scope_cpu_sets[scope] else (),
                    command,
                    result,
                ),
                name=f"hicache-calibration-scope-{scope}",
            )
            process.start()
            self._commands.append(command)
            self._results.append(result)
            self._processes.append(process)
        try:
            for scope in range(scope_count):
                ready = self._results[scope].get(timeout=self._RESULT_TIMEOUT_SEC)
                if not isinstance(ready, dict) or ready.get("status") != "ready":
                    raise RuntimeError(f"storage calibration scope {scope} failed to start: {ready}")
                actual = {int(cpu) for cpu in ready.get("cpu_affinity") or []}
                if scope_cpu_sets[scope] is not None and actual != scope_cpu_sets[scope]:
                    raise RuntimeError(f"storage calibration scope {scope} CPU affinity mismatch")
        except Exception:
            self.close()
            raise

    def run(
        self,
        operation: str,
        keys_by_scope: list[list[str]],
        page_bytes: int,
        operation_bytes_per_scope: int = 0,
    ) -> tuple[list[int], int]:
        if operation not in {"write", "read"}:
            raise ValueError(f"unknown storage scope operation: {operation}")
        if len(keys_by_scope) != len(self._processes):
            raise ValueError("storage scope key partition does not match process count")
        task_id = self._take_task_id()
        start = time.perf_counter_ns()
        for scope, keys in enumerate(keys_by_scope):
            self._commands[scope].put(
                {
                    "task_id": task_id,
                    "operation": operation,
                    "keys": keys,
                    "page_bytes": page_bytes,
                    "operation_bytes_per_scope": operation_bytes_per_scope,
                }
            )
        rows = [self._receive(scope, task_id) for scope in range(len(self._processes))]
        wall_duration_ns = time.perf_counter_ns() - start
        return [int(row["duration_ns"]) for row in rows], wall_duration_ns

    def clear(self) -> None:
        if not self._processes:
            return
        delete_task_id = self._take_task_id()
        self._commands[0].put({"task_id": delete_task_id, "operation": "clear_backend"})
        self._receive(0, delete_task_id)
        reset_task_id = self._take_task_id()
        for command in self._commands:
            command.put({"task_id": reset_task_id, "operation": "reset_evictor"})
        for scope in range(len(self._processes)):
            self._receive(scope, reset_task_id)

    def close(self) -> None:
        processes = list(getattr(self, "_processes", []))
        commands = list(getattr(self, "_commands", []))
        for command in commands:
            try:
                command.put({"operation": "stop"})
            except Exception:
                pass
        for process in processes:
            process.join(timeout=10.0)
            if process.is_alive():
                process.terminate()
                process.join(timeout=10.0)
        self._processes = []
        for queue in commands + list(getattr(self, "_results", [])):
            try:
                queue.close()
            except Exception:
                pass

    def _take_task_id(self) -> int:
        task_id = self._next_task_id
        self._next_task_id += 1
        return task_id

    def _receive(self, scope: int, task_id: int) -> dict[str, Any]:
        row = self._results[scope].get(timeout=self._RESULT_TIMEOUT_SEC)
        if not isinstance(row, dict) or int(row.get("task_id") or -1) != task_id:
            raise RuntimeError(f"storage calibration scope {scope} returned an invalid result: {row}")
        if row.get("status") != "ok":
            raise RuntimeError(f"storage calibration scope {scope} failed: {row.get('error')}")
        return row


def _storage_scope_process_main(
    storage_dir: str,
    scope: int,
    scope_count: int,
    model_name: str,
    cpu_set: tuple[int, ...],
    command_queue: Any,
    result_queue: Any,
) -> None:
    try:
        if cpu_set:
            os.sched_setaffinity(0, set(cpu_set))
        backend = build_hicache_file_backends(
            storage_dir=Path(storage_dir),
            scope_count=scope_count,
            model_name=model_name,
        )[scope]
        import torch

        page_state_cache: dict[str, Any] | None = None
        page_state_geometry: tuple[Any, ...] | None = None
        result_queue.put(
            {
                "status": "ready",
                "scope": scope,
                "cpu_affinity": sorted(os.sched_getaffinity(0)),
            }
        )
        while True:
            request = command_queue.get()
            operation = str(request.get("operation") or "")
            if operation == "stop":
                return
            task_id = int(request.get("task_id") or 0)
            try:
                if operation == "clear_backend":
                    if not backend.clear():
                        raise IOError("HiCacheFile.clear failed")
                    duration_ns = 0
                elif operation == "reset_evictor":
                    backend._evictor.clear()
                    duration_ns = 0
                else:
                    page_bytes = int(request["page_bytes"])
                    operation_bytes_per_scope = int(request.get("operation_bytes_per_scope") or 0)
                    required_source_pages = (
                        operation_bytes_per_scope // page_bytes if operation_bytes_per_scope > 0 else 2
                    )
                    requested_geometry = (page_bytes, required_source_pages)
                    if page_state_cache is None or page_state_geometry != requested_geometry:
                        page_state = create_storage_page_state(
                            torch,
                            page_bytes,
                            pin_memory=True,
                            working_set_page_count=required_source_pages,
                        )
                        page_state_cache = page_state
                        page_state_geometry = requested_geometry
                    else:
                        page_state = page_state_cache
                    start = time.perf_counter_ns()
                    if operation == "write":
                        keys = [str(key) for key in request.get("keys") or []]
                        if operation_bytes_per_scope > 0:
                            write_storage_runtime_batches(
                                backend,
                                keys,
                                page_state,
                                operation_bytes_per_scope=operation_bytes_per_scope,
                            )
                        else:
                            for page_ordinal, key in enumerate(keys):
                                source = materialize_storage_write_page(page_state, page_ordinal)
                                if not backend.set(key, source):
                                    raise IOError(f"HiCacheFile.set failed for calibration key {key}")
                    elif operation == "read":
                        for page_ordinal, key in enumerate(request.get("keys") or []):
                            target = allocate_storage_read_page(page_state)
                            if backend.get(str(key), target) is None:
                                raise IOError(f"HiCacheFile.get failed for calibration key {key}")
                            commit_storage_read_page(page_state, page_ordinal, target)
                    else:
                        raise ValueError(f"unknown storage worker operation: {operation}")
                    duration_ns = time.perf_counter_ns() - start
                result_queue.put(
                    {
                        "task_id": task_id,
                        "status": "ok",
                        "duration_ns": duration_ns,
                    }
                )
            except Exception as exc:
                result_queue.put({"task_id": task_id, "status": "error", "error": repr(exc)})
    except Exception as exc:
        result_queue.put({"status": "error", "scope": scope, "error": repr(exc)})


def calibrate_storage_curves(
    *,
    storage_dir: Path,
    page_sizes: list[int],
    scope_count: int,
    model_name: str,
    warmup: int,
    repeats: int,
    isolated_repeats: int,
    scope_cpu_sets: list[set[int] | None] | None = None,
    existing_operation_pages: list[int] | None = None,
) -> list[dict[str, Any]]:
    process_scopes = StorageScopeProcesses(
        storage_dir=storage_dir,
        scope_count=scope_count,
        model_name=model_name,
        scope_cpu_sets=scope_cpu_sets,
    )
    samples: list[dict[str, Any]] = []
    try:
        for page_bytes in page_sizes:
            for ordinal in range(warmup + isolated_repeats):
                read = timed_storage_batch(
                    direction="storage_to_host",
                    state="isolated_warm",
                    page_bytes=page_bytes,
                    bytes_per_scope=page_bytes,
                    scope_count=scope_count,
                    ordinal=ordinal,
                    process_scopes=process_scopes,
                )
                if ordinal >= warmup:
                    read["ordinal"] = ordinal - warmup
                    samples.append(read)
        samples.extend(
            calibrate_existing_key_curves(
                page_sizes=page_sizes,
                operation_pages=existing_operation_pages or [1, 8, 32, 64],
                scope_count=scope_count,
                warmup=warmup,
                repeats=repeats,
                process_scopes=process_scopes,
            )
        )
    finally:
        process_scopes.close()
    return samples


def calibrate_new_write_only(
    *,
    storage_dir: Path,
    page_sizes: list[int],
    queue_bytes_per_scope: list[int],
    operation_bytes_per_scope: list[int],
    scope_count: int,
    model_name: str,
    warmup: int,
    repeats: int,
    scope_cpu_sets: list[set[int] | None],
) -> list[dict[str, Any]]:
    """Measure only new-key sustained writes over a repeated physical grid."""

    process_scopes = StorageScopeProcesses(
        storage_dir=storage_dir,
        scope_count=scope_count,
        model_name=model_name,
        scope_cpu_sets=scope_cpu_sets,
    )
    samples: list[dict[str, Any]] = []
    try:
        for page_bytes in sorted(page_sizes):
            for queue_bytes in sorted(queue_bytes_per_scope):
                for operation_bytes in sorted(operation_bytes_per_scope):
                    for ordinal in range(warmup + repeats):
                        write = timed_storage_batch(
                            direction="host_to_storage",
                            state="sustained",
                            page_bytes=page_bytes,
                            bytes_per_scope=queue_bytes,
                            operation_bytes_per_scope=operation_bytes,
                            scope_count=scope_count,
                            ordinal=ordinal,
                            process_scopes=process_scopes,
                        )
                        if ordinal >= warmup:
                            write["ordinal"] = ordinal - warmup
                            samples.append(write)
    finally:
        process_scopes.close()
    return samples


def storage_operation_byte_anchors(
    raw: str,
    *,
    burst_bytes_per_scope: int,
    page_sizes: list[int],
) -> list[int]:
    """Resolve a config-independent operation-payload grid.

    The maximum payload is the already-declared physical burst domain.  The
    lower points are fixed fractions of that domain, rounded down to the least
    common multiple of the supported page-byte geometry.  No target config,
    workload identity, or target timing is consulted.
    """

    if not page_sizes or any(page <= 0 for page in page_sizes):
        raise ValueError("storage operation anchors require positive page sizes")
    quantum = math.lcm(*page_sizes)
    if raw.strip():
        anchors = parse_positive_csv(raw, "storage-new-write-operation-bytes-per-scope")
    else:
        if burst_bytes_per_scope <= 0:
            raise ValueError("source burst byte domain is required to derive operation anchors")
        anchors = sorted(
            {max(quantum, (burst_bytes_per_scope * numerator // 3 // quantum) * quantum) for numerator in (1, 2, 3)}
        )
    if any(anchor % page != 0 for anchor in anchors for page in page_sizes):
        raise ValueError("storage operation byte anchors must be divisible by every page size")
    return anchors


def calibrate_existing_key_curves(
    *,
    page_sizes: list[int],
    operation_pages: list[int],
    scope_count: int,
    warmup: int,
    repeats: int,
    process_scopes: StorageScopeProcesses,
) -> list[dict[str, Any]]:
    """Measure flatten + existing-key check after one untimed population per grid point."""

    samples: list[dict[str, Any]] = []
    for page_bytes in page_sizes:
        for pages_per_scope in operation_pages:
            prefix = f"hicache_calibration_{os.getpid()}_existing_{page_bytes}_{pages_per_scope}"
            keys_by_scope = [
                [f"{prefix}_scope{scope}_page{page}" for page in range(pages_per_scope)] for scope in range(scope_count)
            ]

            def execute() -> tuple[list[int], int]:
                return process_scopes.run("write", keys_by_scope, page_bytes)

            try:
                # This is the only physical file-write population for the grid
                # coordinate. All timed repetitions hit the same resident keys.
                execute()
                for ordinal in range(warmup + repeats):
                    per_scope_ns, wall_ns = execute()
                    if ordinal < warmup:
                        continue
                    total_pages = pages_per_scope * scope_count
                    attempted_bytes = total_pages * page_bytes
                    row = sample_row(
                        f"storage/host_to_storage/existing_key/{page_bytes}/{pages_per_scope}",
                        ordinal - warmup,
                        attempted_bytes,
                        wall_ns,
                    )
                    row.update(
                        {
                            "direction": "host_to_storage",
                            "resource_state": "existing_key",
                            "page_bytes": page_bytes,
                            "page_count": total_pages,
                            "scope_count": scope_count,
                            "operation_pages_per_scope": pages_per_scope,
                            "service_duration_ns": sum(per_scope_ns),
                            "page_materialization": STORAGE_PAGE_MATERIALIZATION_RUNTIME,
                        }
                    )
                    samples.append(row)
            finally:
                process_scopes.clear()
    return samples


def timed_storage_batch(
    *,
    direction: str,
    state: str,
    page_bytes: int,
    bytes_per_scope: int,
    operation_bytes_per_scope: int = 0,
    scope_count: int,
    ordinal: int,
    process_scopes: StorageScopeProcesses,
) -> dict[str, Any]:
    """Measure one process-isolated TP storage batch."""

    page_count_per_scope = math.ceil(bytes_per_scope / page_bytes)
    total_bytes = page_count_per_scope * page_bytes * scope_count
    prefix = f"hicache_calibration_{os.getpid()}_{direction}_{state}_{page_bytes}_{ordinal}"
    keys_by_scope = [
        [f"{prefix}_scope{scope}_page{page}" for page in range(page_count_per_scope)] for scope in range(scope_count)
    ]

    try:
        if direction == "storage_to_host":
            process_scopes.run("write", keys_by_scope, page_bytes)
            process_scopes.run("read", keys_by_scope, page_bytes)
            operation = "read"
        elif direction == "host_to_storage":
            operation = "write"
        else:
            raise ValueError(f"unsupported storage calibration direction: {direction}")
        scope_durations_ns, duration_ns = process_scopes.run(
            operation,
            keys_by_scope,
            page_bytes,
            operation_bytes_per_scope=(operation_bytes_per_scope if direction == "host_to_storage" else 0),
        )
    finally:
        process_scopes.clear()

    row = sample_row(f"storage/{direction}/{state}", ordinal, total_bytes, duration_ns)
    row.update(
        {
            "direction": direction,
            "resource_state": state,
            "page_bytes": page_bytes,
            "page_count": page_count_per_scope * scope_count,
            "scope_count": scope_count,
            "operation_bytes_per_scope": (operation_bytes_per_scope if operation_bytes_per_scope > 0 else None),
            "operation_pages_per_scope": (
                operation_bytes_per_scope // page_bytes if operation_bytes_per_scope > 0 else None
            ),
            "operation_count": (
                scope_count * math.ceil(page_count_per_scope / max(1, operation_bytes_per_scope // page_bytes))
                if operation_bytes_per_scope > 0
                else scope_count
            ),
            "batch_semantics": (
                "runtime_materialize_then_batch_set"
                if operation_bytes_per_scope > 0 and direction == "host_to_storage"
                else "page_interleaved"
            ),
            "source_working_set_semantics": (
                STORAGE_SOURCE_WORKING_SET_RUNTIME
                if operation_bytes_per_scope > 0 and direction == "host_to_storage"
                else None
            ),
            "service_duration_ns": sum(scope_durations_ns),
            "page_materialization": STORAGE_PAGE_MATERIALIZATION_RUNTIME,
        }
    )
    return row


def create_storage_page_state(
    torch_module: Any,
    page_bytes: int,
    *,
    pin_memory: bool = True,
    working_set_page_count: int = 2,
) -> dict[str, Any]:
    """Create a byte-equivalent non-contiguous page-first host buffer."""

    if page_bytes <= 0:
        raise ValueError("storage page bytes must be positive")
    if working_set_page_count <= 0:
        raise ValueError("storage source working-set page count must be positive")
    if page_bytes % 2:
        raise ValueError("page_first_direct MHA byte emulation requires even page bytes")
    page_buffer = torch_module.empty(
        (2, working_set_page_count, page_bytes // 2),
        dtype=torch_module.uint8,
        device="cpu",
        pin_memory=pin_memory,
    )
    page_buffer.fill_(0x5A)
    return {
        "mode": STORAGE_PAGE_MATERIALIZATION_RUNTIME,
        "page_bytes": page_bytes,
        "page_slot_count": working_set_page_count,
        "pin_memory": pin_memory,
        "torch": torch_module,
        "page_buffer": page_buffer,
        "page_dtype": torch_module.uint8,
        "page_element_count": page_bytes,
    }


def materialize_storage_write_page(page_state: dict[str, Any], page_ordinal: int) -> Any:
    slot = int(page_ordinal) % int(page_state["page_slot_count"])
    page_view = page_state["page_buffer"][:, slot : slot + 1, :]
    if page_view.is_contiguous():
        raise RuntimeError("page_first_direct write emulation unexpectedly became contiguous")
    flat_page = page_view.flatten()
    if (
        not flat_page.is_contiguous()
        or flat_page.numel() != page_state["page_element_count"]
        or flat_page.numel() * flat_page.element_size() != page_state["page_bytes"]
    ):
        raise RuntimeError("page_first_direct write emulation produced an invalid flat page")
    return flat_page


def write_storage_runtime_batches(
    backend: Any,
    keys: list[str],
    page_state: dict[str, Any],
    *,
    operation_bytes_per_scope: int,
) -> int:
    """Execute the deployed `_generic_page_set` ordering for a queue.

    Runtime materializes every page in one operation before `batch_set` starts
    writing.  Keeping that ordering matters because the flattened page tensors
    remain live together and exercise allocator/cache pressure that a
    page-at-a-time calibration silently omits.
    """

    page_bytes = int(page_state["page_bytes"])
    if operation_bytes_per_scope <= 0 or operation_bytes_per_scope % page_bytes:
        raise ValueError("runtime storage operation bytes must be a positive page multiple")
    pages_per_operation = operation_bytes_per_scope // page_bytes
    if int(page_state.get("page_slot_count") or 0) < pages_per_operation:
        raise ValueError("runtime storage operation requires one distinct source slot per page")
    operation_count = 0
    for start in range(0, len(keys), pages_per_operation):
        batch_keys = keys[start : start + pages_per_operation]
        batch_values = [
            materialize_storage_write_page(page_state, page_ordinal)
            for page_ordinal in range(start, start + len(batch_keys))
        ]
        if not backend.batch_set(batch_keys, batch_values):
            raise IOError(f"HiCacheFile.batch_set failed for {len(batch_keys)} calibration keys")
        operation_count += 1
    return operation_count


def allocate_storage_read_page(page_state: dict[str, Any]) -> Any:
    torch_module = page_state["torch"]
    return torch_module.zeros(
        page_state["page_element_count"],
        dtype=page_state["page_dtype"],
        device="cpu",
        pin_memory=page_state["pin_memory"],
    )


def commit_storage_read_page(page_state: dict[str, Any], page_ordinal: int, flat_page: Any) -> None:
    if (
        flat_page.numel() != page_state["page_element_count"]
        or flat_page.numel() * flat_page.element_size() != page_state["page_bytes"]
    ):
        raise ValueError("storage read page byte count does not match page state")
    slot = int(page_ordinal) % int(page_state["page_slot_count"])
    destination = page_state["page_buffer"][:, slot : slot + 1, :]
    destination.copy_(flat_page.reshape(destination.shape))


def build_hicache_file_backends(*, storage_dir: Path, scope_count: int, model_name: str) -> list[Any]:
    """Instantiate the exact runtime file backend once per TP scope."""

    from sglang.srt.mem_cache.hicache_storage import HiCacheFile, HiCacheStorageConfig

    backends = []
    for scope in range(scope_count):
        config = HiCacheStorageConfig(
            tp_rank=scope,
            tp_size=scope_count,
            pp_rank=0,
            pp_size=1,
            attn_cp_rank=0,
            attn_cp_size=1,
            is_mla_model=False,
            enable_storage_metrics=False,
            is_page_first_layout=True,
            model_name=model_name,
        )
        backends.append(HiCacheFile(config, file_path=str(storage_dir)))
    return backends
