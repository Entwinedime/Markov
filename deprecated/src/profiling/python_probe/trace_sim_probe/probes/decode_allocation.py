"""Observe extend/decode/load allocation boundaries without tensor reads."""

import os
import time
from functools import lru_cache
from types import ModuleType
from typing import Any, Callable

from trace_sim_probe.patching import install_wrapper
from trace_sim_probe.probes.hicache.common import _cache_scope_key
from trace_sim_probe.writer import get_writer

TARGET_MODULES = (
    "sglang.srt.mem_cache.common",
    "sglang.srt.managers.schedule_batch",
    "sglang.srt.managers.cache_controller",
    "sglang.srt.mem_cache.hiradix_cache",
    "sglang.srt.hardware_backend.npu.allocator_npu",
)
_MARKER = "__trace_sim_decode_allocation__"
_CAPACITY_MARKER = "__trace_sim_capacity_guard__"
_LOAD_MARKER = "__trace_sim_load_allocation__"
_ADMISSION_MARKER = "__trace_sim_load_admission__"
_EXTEND_MARKER = "__trace_sim_extend_allocation__"
_ALLOCATOR_MARKER = "__trace_sim_allocator_call__"


def _npu_allocator_call(
    original: Callable[..., Any],
    phase: str,
    device_name: Callable[[Any], str],
) -> Callable[..., Any]:
    def observed(allocator: Any, *args: Any, **kwargs: Any) -> Any:
        # This ABI is the declared NPU paged allocator. Shape/dtype access does
        # not copy tensor values or synchronize the device. Request identity
        # comes from the enclosing alloc_for_extend/decode observation.
        sequence = kwargs["seq_lens"] if "seq_lens" in kwargs else args[0 if phase == "decode" else 2]
        last_location = kwargs["last_loc"] if "last_loc" in kwargs else args[2 if phase == "decode" else 4]
        fields = {
            "allocator_implementation": f"{type(allocator).__module__}.{type(allocator).__qualname__}",
            "device_name": device_name(sequence.device),
            "page_size_tokens": allocator.page_size,
            "batch_size": sequence.shape[0],
            "sequence_dtype": str(sequence.dtype),
            "last_location_dtype": str(last_location.dtype),
            "debug_memory_pool": allocator.debug_mode,
            "allocator_need_sort": allocator.need_sort,
            "free_pages_before": len(allocator.free_pages),
            "release_pages_before": len(allocator.release_pages),
            "free_page_dtype": str(allocator.free_pages.dtype),
            "free_page_stride_elements": allocator.free_pages.stride(0),
            "free_page_storage_offset_elements": allocator.free_pages.storage_offset(),
            "status": "raised",
        }
        if phase == "extend":
            prefix = kwargs["prefix_lens"] if "prefix_lens" in kwargs else args[0]
            # Match the installed Ascend Triton launcher's actual switch,
            # including its default. Disabled means direct launch + sync,
            # not the same tensor call with a different service duration.
            fields["triton_task_queue_enabled"] = os.getenv("TRITON_ENABLE_TASKQUEUE", "true").lower() in {"true", "1"}
            fields["prefix_dtype"] = str(prefix.dtype)
            fields["extend_tokens"] = kwargs["extend_num_tokens"] if "extend_num_tokens" in kwargs else args[5]
            fields["supplied_new_pages"] = kwargs.get("num_new_pages", args[6] if len(args) > 6 else None)

        writer = get_writer()
        start_us = writer.now_us()
        cpu_start_ns = time.thread_time_ns()
        cpu_end_ns = None
        try:
            result = original(allocator, *args, **kwargs)
            cpu_end_ns = time.thread_time_ns()
            fields.update(status="returned", allocated=result is not None)
            # Queue sizes establish the actual merge/consume transition without
            # reading page identities or synchronizing a device tensor.
            fields["free_pages_after"] = len(allocator.free_pages)
            fields["release_pages_after"] = len(allocator.release_pages)
            if result is not None:
                fields["output_dtype"] = str(result.dtype)
            return result
        finally:
            if cpu_end_ns is None:
                cpu_end_ns = time.thread_time_ns()
            fields["thread_cpu_ns"] = cpu_end_ns - cpu_start_ns
            writer.duration_event(
                f"runtime.hicache.{phase}_allocator", start_us, writer.now_us(), "runtime_diagnostic", fields
            )

    return observed


def _extend_allocation(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(batch: Any) -> Any:
        allocator = batch.tree_cache.token_to_kv_pool_allocator
        page_size = allocator.page_size
        # These lists and request lengths already exist before allocation.
        # Do not read seq_lens_cpu, output indices or tensor contents.
        prefix_tokens = list(batch.prefix_lens)
        sequence_tokens = [req.fill_len for req in batch.reqs]
        fields = {
            "request_ids": [req.rid for req in batch.reqs],
            "prefix_lens_tokens": prefix_tokens,
            "sequence_lens_tokens": sequence_tokens,
            "extend_lens_tokens": list(batch.extend_lens),
            "extend_tokens": batch.extend_num_tokens,
            "page_size_tokens": page_size,
            "new_pages": sum(
                (length + page_size - 1) // page_size - (prefix + page_size - 1) // page_size
                for length, prefix in zip(sequence_tokens, prefix_tokens)
            ),
            "allocator_implementation": f"{type(allocator).__module__}.{type(allocator).__qualname__}",
            "free_page_dtype": str(allocator.free_pages.dtype),
            "free_page_stride_elements": allocator.free_pages.stride(0),
            "status": "raised",
        }
        writer = get_writer()
        start_us = writer.now_us()
        try:
            result = original(batch)
            fields["status"] = "returned"
            return result
        finally:
            writer.duration_event(
                "runtime.hicache.extend_allocation", start_us, writer.now_us(), "runtime_diagnostic", fields
            )

    return observed


def _load_admission(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(owner: Any, params: Any) -> Any:
        # Shape metadata only: do not inspect index values, hash pages or
        # cache snapshots. This work description is for offline measurements,
        # never a source-hit template for target structure generation.
        node = params.best_match_node
        node_tokens: list[int] = []
        while node.evicted:
            node_tokens.append(len(node.host_value))
            node = node.parent
        req = params.req
        allocator = owner.cache_controller.mem_pool_device_allocator
        fields = {
            "request_id": req.rid if req is not None else "",
            "existing_prefix_tokens": len(req.prefix_indices) if req is not None else None,
            "host_node_tokens": list(reversed(node_tokens)),
            "requested_tokens": sum(node_tokens),
            "page_size": allocator.page_size,
            "allocator_need_sort": allocator.need_sort,
            "load_back_threshold_tokens": owner.load_back_threshold,
            "mem_quota_tokens": params.mem_quota,
            "status": "raised",
        }
        writer = get_writer()
        start = writer.now_us()
        try:
            result = original(owner, params)
            fields.update(status="returned", returned_tokens=len(result[0]))
            return result
        finally:
            writer.duration_event(
                "runtime.hicache.load_admission", start, writer.now_us(), "runtime_diagnostic", fields
            )

    return observed


def _load_attempt(original):
    def observed(controller, *args, **kwargs):
        # A successful return queues work; it is not device completion. Merely
        # test object identity against None: never inspect the returned tensor.
        fields = {"node_id": kwargs.get("node_id", args[2] if len(args) > 2 else -1), "status": "raised"}
        host_indices = kwargs["host_indices"] if "host_indices" in kwargs else args[0]
        allocator = controller.mem_pool_device_allocator
        fields.update(
            requested_tokens=len(host_indices),
            page_size=allocator.page_size,
            allocator_need_sort=allocator.need_sort,
            free_pages_before=len(allocator.free_pages),
            release_pages_before=len(allocator.release_pages),
        )
        writer = get_writer()
        start = writer.now_us()
        cpu_start_ns = time.thread_time_ns()
        cpu_end_ns = None
        try:
            result = original(controller, *args, **kwargs)
            cpu_end_ns = time.thread_time_ns()
            fields.update(status="returned", allocated=result is not None)
            return result
        finally:
            if cpu_end_ns is None:
                cpu_end_ns = time.thread_time_ns()
            fields["thread_cpu_ns"] = cpu_end_ns - cpu_start_ns
            writer.duration_event(
                "runtime.hicache.load_allocation", start, writer.now_us(), "runtime_diagnostic", fields
            )

    return observed


def _capacity_guard(original):
    def observed(*args, **kwargs):
        # Observe the opportunity even when the base has enough free capacity.
        # Target cache state, not a sampled source decision, decides eviction.
        writer = get_writer()
        start = writer.now_us()
        status = "raised"
        try:
            result = original(*args, **kwargs)
            status = "returned"
            return result
        finally:
            writer.duration_event(
                "runtime.hicache.capacity_guard", start, writer.now_us(), "runtime_diagnostic", {"status": status}
            )

    return observed


def _allocation(original):
    def observed(batch, token_per_req):
        # These are existing Python scalars, captured before allocation and before
        # prepare_for_decode increments them. Never touch seq_lens or the result.
        page_size = batch.tree_cache.token_to_kv_pool_allocator.page_size
        prefixes = [req.kv_allocated_len for req in batch.reqs]
        sequences = [length + token_per_req for length in prefixes]
        fields = {
            "cache_scope": _cache_scope_key(batch.tree_cache),
            "token_per_req": token_per_req,
            "request_ids": [req.rid for req in batch.reqs],
            "prefix_lens_tokens": prefixes,
            "sequence_lens_tokens": sequences,
            "page_size_tokens": page_size,
            "extend_tokens": len(prefixes) * token_per_req,
            "new_pages": sum(
                (length + page_size - 1) // page_size - (prefix + page_size - 1) // page_size
                for length, prefix in zip(sequences, prefixes)
            ),
            "requests": [
                {
                    "request_id": req.rid,
                    "kv_committed_len": req.kv_committed_len,
                    "kv_allocated_len": req.kv_allocated_len,
                    "decode_batch_idx": req.decode_batch_idx,
                }
                for req in batch.reqs
            ],
            "status": "raised",
        }
        writer = get_writer()
        start = writer.now_us()
        try:
            result = original(batch, token_per_req)
            fields["status"] = "returned"
            return result
        finally:
            writer.duration_event(
                "runtime.hicache.decode_allocation", start, writer.now_us(), "runtime_diagnostic", fields
            )

    return observed


def install(module: ModuleType) -> None:
    """Cover the definition and the scheduler's imported alias, once per call."""
    if module.__name__ == "sglang.srt.hardware_backend.npu.allocator_npu":
        allocator = getattr(module, "NPUPagedTokenToKVPoolAllocator", None)

        # Query device metadata on the first real allocation, after the runtime
        # has initialized, not during probe installation. No tensor is read.
        device_name = lru_cache(maxsize=None)(module.torch.npu.get_device_name)
        install_wrapper(
            allocator,
            "alloc_extend",
            lambda original: _npu_allocator_call(original, "extend", device_name),
            _ALLOCATOR_MARKER,
        )
        install_wrapper(
            allocator,
            "alloc_decode",
            lambda original: _npu_allocator_call(original, "decode", device_name),
            _ALLOCATOR_MARKER,
        )
        return

    if module.__name__ == "sglang.srt.mem_cache.hiradix_cache":
        install_wrapper(getattr(module, "HiRadixCache", None), "init_load_back", _load_admission, _ADMISSION_MARKER)
        return

    if module.__name__ == "sglang.srt.managers.cache_controller":
        controller = getattr(module, "HiCacheController", None)
        install_wrapper(controller, "load", _load_attempt, _LOAD_MARKER)
        return

    if module.__name__ not in TARGET_MODULES:
        return
    install_wrapper(module, "alloc_for_decode", _allocation, _MARKER)
    install_wrapper(module, "alloc_for_extend", _extend_allocation, _EXTEND_MARKER)

    if module.__name__ == "sglang.srt.mem_cache.common":
        install_wrapper(module, "evict_from_tree_cache", _capacity_guard, _CAPACITY_MARKER)
