"""Process-isolated host-storage calibration sampling."""

from __future__ import annotations

import math
import multiprocessing
import os
import time
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace
from typing import Any

from ...common.io import write_json
from .options import STORAGE_PAGE_MATERIALIZATION_RUNTIME, apply_numa_preference, parse_positive_csv
from ..physical_calibration import sample_row


STORAGE_SOURCE_WORKING_SET_RUNTIME = "operation_payload_distinct_page_sources"


@dataclass(frozen=True)
class HostStorageCapturePlan:
    """Validated physical grid consumed by the storage sampler."""

    storage_dir: Path
    page_sizes: tuple[int, ...]
    devices: tuple[int, ...]
    numa_nodes: tuple[int | None, ...]
    model_name: str
    kv_geometry: dict[str, Any]
    warmup: int
    repeats: int
    scope_cpu_sets: tuple[frozenset[int] | None, ...]
    existing_operation_pages: tuple[int, ...]
    new_write_queues: tuple[int, ...]
    new_write_operations: tuple[int, ...]
    service: str


def capture_host_storage(plan: HostStorageCapturePlan, *, observations_path: Path) -> dict[str, Any]:
    """Measure one admitted service with one worker per deployed rank.

    Persist raw observations before fitting, so a fit failure does not discard
    the measurement. The public options parser admits the service name.
    """
    from .prefetch_service import capture_prefetch_timings

    workers = StorageScopeProcesses(plan)
    try:
        storage_samples, prefetch_samples = [], []
        if plan.service == "existing_write":
            storage_samples = calibrate_existing_key_curves(plan, workers)
        elif plan.service == "new_write":
            storage_samples = calibrate_new_write_only(plan, workers)
        else:
            prefetch_samples = capture_prefetch_timings(
                workers, list(plan.page_sizes), warmup=plan.warmup, repeats=plan.repeats
            )
        observations = {
            "host_storage": {"samples": storage_samples},
            "prefetch_service_observations": prefetch_samples,
        }
        write_json(observations_path, {"status": "captured", **observations})
        return observations
    finally:
        workers.close()


class StorageScopeProcesses:
    """Persistent one-process-per-scope executor for storage calibration.

    SGLang deploys TP ranks as separate processes.  A thread pool in one Python
    process introduces a shared interpreter and does not reproduce that resource
    state, so the default calibration path mirrors the rank process boundary.
    """

    _RESULT_TIMEOUT_SEC = 600.0

    def __init__(self, plan: HostStorageCapturePlan) -> None:
        scope_count = len(plan.devices)
        if not plan.devices or len(set(plan.devices)) != scope_count or any(device < 0 for device in plan.devices):
            raise ValueError("storage calibration requires distinct nonnegative deployment devices")
        if len(plan.scope_cpu_sets) != scope_count:
            raise ValueError("storage scope CPU set count must match scope count")
        if len(plan.numa_nodes) != scope_count:
            raise ValueError("storage NUMA node count must match scope count")
        context = multiprocessing.get_context("spawn")
        self._processes: list[multiprocessing.Process] = []
        self._commands: list[Any] = []
        self._results: list[Any] = []
        self.worker_threads: list[int] = []
        self.worker_devices: list[int] = []
        self.worker_numa_nodes: list[int | None] = []
        self.worker_storage_batch_pages: list[int] = []
        self._next_task_id = 1
        try:
            for scope in range(scope_count):
                command = context.Queue()
                self._commands.append(command)
                result = context.Queue()
                self._results.append(result)
                process = context.Process(
                    target=_storage_scope_process_main,
                    args=(plan, scope, command, result),
                    name=f"hicache-calibration-scope-{scope}",
                )
                self._processes.append(process)
                process.start()

            for scope in range(scope_count):
                ready = self._results[scope].get(timeout=self._RESULT_TIMEOUT_SEC)
                if not isinstance(ready, dict) or ready.get("status") != "ready":
                    raise RuntimeError(f"storage calibration scope {scope} failed to start: {ready}")
                self.worker_threads.append(int(ready["torch_num_threads"]))
                self.worker_devices.append(int(ready["device"]))
                self.worker_numa_nodes.append(ready["numa_preferred_node"])
                self.worker_storage_batch_pages.append(int(ready["storage_batch_pages"]))
                if self.worker_devices[-1] != plan.devices[scope]:
                    raise RuntimeError(f"storage calibration scope {scope} selected a different device")
                actual = {int(cpu) for cpu in ready.get("cpu_affinity") or []}
                if plan.scope_cpu_sets[scope] is not None and actual != plan.scope_cpu_sets[scope]:
                    raise RuntimeError(f"storage calibration scope {scope} CPU affinity mismatch")
        except BaseException:
            self.close()
            raise

    def write(
        self,
        keys_by_scope: list[list[str]],
        page_bytes: int,
        operation_bytes_per_scope: int = 0,
    ) -> tuple[list[int], int]:
        """Write each rank's keys; return rank service and total wall times in ns."""

        if len(keys_by_scope) != len(self._processes):
            raise ValueError("storage scope key partition does not match process count")
        task_id = self._take_task_id()
        start = time.perf_counter_ns()
        for scope, keys in enumerate(keys_by_scope):
            self._commands[scope].put(
                {
                    "task_id": task_id,
                    "operation": "write",
                    "keys": keys,
                    "page_bytes": page_bytes,
                    "operation_bytes_per_scope": operation_bytes_per_scope,
                }
            )
        rows = self._receive_all(task_id)
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
        self._receive_all(reset_task_id)

    def observe_prefetch(
        self, keys_by_scope: list[list[str]], page_bytes: int, *, cancel_after_pages: int | None = None
    ) -> list[dict]:
        """Separate instrumented calls; never mix these with bandwidth samples."""
        if len(keys_by_scope) != len(self._processes):
            raise ValueError("prefetch timing scope count must match workers")
        task_id = self._take_task_id()
        for scope, keys in enumerate(keys_by_scope):
            self._commands[scope].put(
                {
                    "task_id": task_id,
                    "operation": "prefetch_timing",
                    "keys": keys,
                    "page_bytes": page_bytes,
                    "cancel_after_pages": cancel_after_pages,
                }
            )
        return [row["prefetch_timing"] for row in self._receive_all(task_id)]

    def close(self) -> None:
        processes, self._processes = self._processes, []
        commands, self._commands = self._commands, []
        results, self._results = self._results, []

        for process, command in zip(processes, commands):
            if process.pid is None:
                continue
            try:
                command.put({"operation": "stop"})
            except Exception:
                pass

        for process in processes:
            if process.pid is None:
                continue
            process.join(timeout=10.0)
            if process.is_alive():
                process.terminate()
                process.join(timeout=10.0)

        for queue in commands + results:
            try:
                queue.close()
            except Exception:
                pass

    def _take_task_id(self) -> int:
        task_id = self._next_task_id
        self._next_task_id += 1
        return task_id

    def _receive_all(self, task_id: int) -> list[dict[str, Any]]:
        """Drain every rank before raising, so later cleanup sees its own replies."""
        rows, errors = [], []
        for scope in range(len(self._processes)):
            try:
                rows.append(self._receive(scope, task_id))
            except RuntimeError as error:
                errors.append(str(error))

        if errors:
            raise RuntimeError("; ".join(errors))
        return rows

    def _receive(self, scope: int, task_id: int) -> dict[str, Any]:
        row = self._results[scope].get(timeout=self._RESULT_TIMEOUT_SEC)
        if not isinstance(row, dict) or int(row.get("task_id") or -1) != task_id:
            raise RuntimeError(f"storage calibration scope {scope} returned an invalid result: {row}")
        if row.get("status") != "ok":
            raise RuntimeError(f"storage calibration scope {scope} failed: {row.get('error')}")
        return row


def _storage_scope_process_main(
    plan: HostStorageCapturePlan,
    scope: int,
    command_queue: Any,
    result_queue: Any,
) -> None:
    try:
        cpu_set = plan.scope_cpu_sets[scope]
        if cpu_set:
            os.sched_setaffinity(0, set(cpu_set))
        actual_numa_node = apply_numa_preference(plan.numa_nodes[scope])
        import torch
        import torch_npu  # noqa: F401
        from sglang.srt.managers.cache_controller import STORAGE_BATCH_SIZE, HiCacheController, PrefetchOperation

        from .prefetch_service import observe_prefetch_transfer

        # Pinned host allocations initialize accelerator context too. Select
        # the same device as this deployment rank before creating any buffers.
        torch.npu.set_device(plan.devices[scope])
        backend = build_hicache_file_backend(
            storage_dir=plan.storage_dir,
            scope=scope,
            scope_count=len(plan.devices),
            model_name=plan.model_name,
        )
        # SGLang ModelRunner uses one intra-op CPU thread on accelerator deployments.
        # Affinity alone leaves PyTorch's machine-wide thread default unchanged.
        torch.set_num_threads(1)
        page_state_cache: dict[str, Any] | None = None
        page_state_geometry: tuple[Any, ...] | None = None
        result_queue.put(
            {
                "status": "ready",
                "scope": scope,
                "cpu_affinity": sorted(os.sched_getaffinity(0)),
                "torch_num_threads": torch.get_num_threads(),
                "device": torch.npu.current_device(),
                "numa_preferred_node": actual_numa_node,
                "storage_batch_pages": STORAGE_BATCH_SIZE,
            }
        )
        while True:
            request = command_queue.get()
            operation = str(request.get("operation") or "")
            if operation == "stop":
                return
            task_id = int(request.get("task_id") or 0)
            try:
                prefetch_timing = None
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
                        max(2, operation_bytes_per_scope // page_bytes) if operation_bytes_per_scope > 0 else 2
                    )
                    if operation == "prefetch_timing":
                        required_source_pages = max(2, min(len(request["keys"]), STORAGE_BATCH_SIZE))
                    # A one-page operation still reads a strided view of the
                    # multi-page runtime pool; the padding page is not transferred.
                    requested_geometry = (page_bytes, required_source_pages)
                    if page_state_cache is None or page_state_geometry != requested_geometry:
                        page_state = create_storage_page_state(
                            torch,
                            page_bytes,
                            pin_memory=True,
                            working_set_page_count=required_source_pages,
                            kv_geometry=plan.kv_geometry,
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
                            pool = page_state["runtime_pool"]
                            for page_ordinal, key in enumerate(keys):
                                slot = page_ordinal % page_state["page_slot_count"]
                                source = pool.get_data_page(page_state["token_indices"][slot * pool.page_size])
                                if not backend.set(key, source):
                                    raise IOError(f"HiCacheFile.set failed for calibration key {key}")
                    elif operation == "prefetch_timing":
                        pool = page_state["runtime_pool"]
                        controller = SimpleNamespace(
                            mem_pool_host=pool, storage_backend=backend, page_size=pool.page_size, has_draft=False
                        )
                        controller.page_get_func = HiCacheController._generic_page_get.__get__(controller)
                        indices = torch.arange(len(request["keys"]) * pool.page_size, dtype=torch.int64) % (
                            required_source_pages * pool.page_size
                        )
                        prefetch = PrefetchOperation("physical_calibration", indices, [])
                        prefetch.hash_value = request["keys"]
                        prefetch_timing = observe_prefetch_transfer(
                            controller,
                            prefetch,
                            HiCacheController._page_transfer,
                            cancel_after_pages=request["cancel_after_pages"],
                        )
                    else:
                        raise ValueError(f"unknown storage worker operation: {operation}")
                    duration_ns = time.perf_counter_ns() - start
                result_queue.put(
                    {
                        "task_id": task_id,
                        "status": "ok",
                        "duration_ns": duration_ns,
                        **({"prefetch_timing": prefetch_timing} if prefetch_timing is not None else {}),
                    }
                )
            except Exception as exc:
                result_queue.put({"task_id": task_id, "status": "error", "error": repr(exc)})
    except Exception as exc:
        result_queue.put({"status": "error", "scope": scope, "error": repr(exc)})


def calibrate_new_write_only(
    plan: HostStorageCapturePlan, process_scopes: StorageScopeProcesses
) -> list[dict[str, Any]]:
    """Measure only new-key sustained writes over a repeated physical grid."""

    samples: list[dict[str, Any]] = []
    for page_bytes in sorted(plan.page_sizes):
        for queue_bytes in sorted(plan.new_write_queues):
            for operation_bytes in sorted(plan.new_write_operations):
                for ordinal in range(plan.warmup + plan.repeats):
                    write = timed_new_write_batch(
                        page_bytes=page_bytes,
                        bytes_per_scope=queue_bytes,
                        operation_bytes_per_scope=operation_bytes,
                        scope_count=len(plan.devices),
                        ordinal=ordinal,
                        process_scopes=process_scopes,
                    )
                    if ordinal >= plan.warmup:
                        write["ordinal"] = ordinal - plan.warmup
                        samples.append(write)
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
    plan: HostStorageCapturePlan,
    process_scopes: StorageScopeProcesses,
) -> list[dict[str, Any]]:
    """Measure runtime batch materialization + existing-key checks after population."""

    samples: list[dict[str, Any]] = []
    scope_count = len(plan.devices)
    for page_bytes in plan.page_sizes:
        for pages_per_scope in plan.existing_operation_pages:
            prefix = f"hicache_calibration_{os.getpid()}_existing_{page_bytes}_{pages_per_scope}"
            keys_by_scope = [
                [f"{prefix}_scope{scope}_page{page}" for page in range(pages_per_scope)] for scope in range(scope_count)
            ]

            def execute() -> tuple[list[int], int]:
                # Existing keys skip file writes, not the controller's whole-batch
                # materialization. Use distinct source slots and keep all pages
                # alive until batch_set returns, just as for new-key operations.
                return process_scopes.write(keys_by_scope, page_bytes, pages_per_scope * page_bytes)

            try:
                # This is the only physical file-write population for the grid
                # coordinate. All timed repetitions hit the same resident keys.
                execute()
                for ordinal in range(plan.warmup + plan.repeats):
                    per_scope_ns, wall_ns = execute()
                    if ordinal < plan.warmup:
                        continue
                    row = _storage_write_sample(
                        resource_state="existing_key",
                        ordinal=ordinal - plan.warmup,
                        page_bytes=page_bytes,
                        pages_per_scope=pages_per_scope,
                        operation_bytes_per_scope=pages_per_scope * page_bytes,
                        scope_durations_ns=per_scope_ns,
                        wall_ns=wall_ns,
                        process_scopes=process_scopes,
                    )
                    samples.append(row)
            finally:
                process_scopes.clear()
    return samples


def timed_new_write_batch(
    *,
    page_bytes: int,
    bytes_per_scope: int,
    operation_bytes_per_scope: int,
    scope_count: int,
    ordinal: int,
    process_scopes: StorageScopeProcesses,
) -> dict[str, Any]:
    """Measure new-key writes through the runtime's whole-batch materialization."""

    page_count_per_scope = math.ceil(bytes_per_scope / page_bytes)
    prefix = f"hicache_calibration_{os.getpid()}_new_write_{page_bytes}_{ordinal}"
    keys_by_scope = [
        [f"{prefix}_scope{scope}_page{page}" for page in range(page_count_per_scope)] for scope in range(scope_count)
    ]

    try:
        scope_durations_ns, duration_ns = process_scopes.write(
            keys_by_scope,
            page_bytes,
            operation_bytes_per_scope=operation_bytes_per_scope,
        )
    finally:
        process_scopes.clear()

    row = _storage_write_sample(
        resource_state="sustained",
        ordinal=ordinal,
        page_bytes=page_bytes,
        pages_per_scope=page_count_per_scope,
        operation_bytes_per_scope=operation_bytes_per_scope,
        scope_durations_ns=scope_durations_ns,
        wall_ns=duration_ns,
        process_scopes=process_scopes,
    )
    row["storage_batch_pages_by_scope"] = list(process_scopes.worker_storage_batch_pages)
    return row


def _storage_write_sample(
    *,
    resource_state: str,
    ordinal: int,
    page_bytes: int,
    pages_per_scope: int,
    operation_bytes_per_scope: int,
    scope_durations_ns: list[int],
    wall_ns: int,
    process_scopes: StorageScopeProcesses,
) -> dict[str, Any]:
    """Record attempted bytes and summed rank service separately from wall time."""

    scope_count = len(scope_durations_ns)
    operation_pages = operation_bytes_per_scope // page_bytes
    group = f"storage/host_to_storage/{resource_state}"
    if resource_state == "existing_key":
        group += f"/{page_bytes}/{pages_per_scope}"

    row = sample_row(group, ordinal, pages_per_scope * page_bytes * scope_count, wall_ns)
    row.update(
        {
            "direction": "host_to_storage",
            "resource_state": resource_state,
            "page_bytes": page_bytes,
            "page_count": pages_per_scope * scope_count,
            "scope_count": scope_count,
            "operation_bytes_per_scope": operation_bytes_per_scope,
            "operation_pages_per_scope": operation_pages,
            "operation_count": scope_count * math.ceil(pages_per_scope / operation_pages),
            "batch_semantics": "runtime_materialize_then_batch_set",
            "source_working_set_semantics": STORAGE_SOURCE_WORKING_SET_RUNTIME,
            "service_duration_ns": sum(scope_durations_ns),
            "scope_cpu_threads": list(process_scopes.worker_threads),
            "scope_devices": list(process_scopes.worker_devices),
            "scope_numa_nodes": list(process_scopes.worker_numa_nodes),
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
    kv_geometry: dict[str, Any],
) -> dict[str, Any]:
    """Create the deployed model's page-first pool for storage calibration."""

    # Framework imports stay local so planning needs neither SGLang nor a device runtime.
    from sglang.srt.managers.cache_controller import HiCacheController
    from sglang.srt.mem_cache.memory_pool_host import MHATokenToKVPoolHost

    if page_bytes <= 0:
        raise ValueError("storage page bytes must be positive")
    if working_set_page_count <= 0:
        raise ValueError("storage source working-set page count must be positive")
    if page_bytes % 2:
        raise ValueError("page-first KV buffers require even page bytes")
    page_buffer = torch_module.empty(
        (2, working_set_page_count, page_bytes // 2),
        dtype=torch_module.uint8,
        device="cpu",
        pin_memory=pin_memory,
    )
    page_buffer.fill_(0x5A)

    class PageView(SimpleNamespace):
        get_data_page = MHATokenToKVPoolHost.get_data_page
        get_dummy_flat_data_page = MHATokenToKVPoolHost.get_dummy_flat_data_page
        set_from_flat_data_page = MHATokenToKVPoolHost.set_from_flat_data_page

    dtype = getattr(torch_module, kv_geometry["kv_torch_dtype"])
    element_bytes = torch_module.empty(0, dtype=dtype).element_size()
    token_bytes = kv_geometry["kv_bytes_per_token_per_rank"]
    if element_bytes != kv_geometry["kv_element_bytes"] or page_bytes % token_bytes:
        raise ValueError("storage page geometry must match the declared runtime KV dtype and token width")
    page_size = page_bytes // token_bytes
    page_buffer = page_buffer.view(dtype).reshape(
        2,
        working_set_page_count,
        kv_geometry["num_hidden_layers"],
        page_size,
        kv_geometry["num_key_value_heads_per_rank"],
        kv_geometry["head_dim"],
    )
    return dict(
        page_bytes=page_bytes,
        page_slot_count=working_set_page_count,
        runtime_pool=PageView(
            kv_buffer=page_buffer,
            page_size=page_size,
            layout="page_first_direct",
            layer_num=kv_geometry["num_hidden_layers"],
            head_num=kv_geometry["num_key_value_heads_per_rank"],
            head_dim=kv_geometry["head_dim"],
            dtype=dtype,
            device="cpu",
            pin_memory=pin_memory,
        ),
        # Repeated indices allow an operation to wrap through the declared
        # source slots without allocating index tensors in the timed call.
        token_indices=torch_module.arange(working_set_page_count * page_size, dtype=torch_module.int64).repeat(2),
        runtime_batch_set=HiCacheController._generic_page_set,
    )


def write_storage_runtime_batches(
    backend: Any,
    keys: list[str],
    page_state: dict[str, Any],
    *,
    operation_bytes_per_scope: int,
) -> None:
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
    for start in range(0, len(keys), pages_per_operation):
        batch_keys = keys[start : start + pages_per_operation]
        pool = page_state["runtime_pool"]
        offset = (start % page_state["page_slot_count"]) * pool.page_size
        indices = page_state["token_indices"][offset : offset + len(batch_keys) * pool.page_size]
        controller = SimpleNamespace(mem_pool_host=pool, page_size=pool.page_size, storage_backend=backend)
        succeeded = page_state["runtime_batch_set"](controller, batch_keys, indices)
        if not succeeded:
            raise IOError(f"HiCacheFile.batch_set failed for {len(batch_keys)} calibration keys")


def build_hicache_file_backend(*, storage_dir: Path, scope: int, scope_count: int, model_name: str) -> Any:
    """Instantiate only this worker's deployed TP file backend."""

    from sglang.srt.mem_cache.hicache_storage import HiCacheFile, HiCacheStorageConfig

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
    return HiCacheFile(config, file_path=str(storage_dir))
