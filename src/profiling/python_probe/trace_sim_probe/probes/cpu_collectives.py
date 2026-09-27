"""CPU collective and prefetch observations required by the DAG consumer.

Record metadata only: no tensor contents, snapshots or extra synchronization.
An async return is submission, and a coalesced call may not issue work at all.
"""

import inspect
import sys
from contextvars import ContextVar
from functools import partial
from types import ModuleType
from typing import Any, Callable

from trace_sim_probe.patching import install_wrapper
from trace_sim_probe.writer import get_writer

_SCOPES = {
    "sglang.srt.managers.scheduler": (
        ("Scheduler", "get_new_batch_prefill", "scheduler_phase", "prefill_selection"),
        ("Scheduler", "update_running_batch", "scheduler_phase", "decode_update"),
    ),
    "sglang.srt.managers.scheduler_components.request_receiver": (
        ("SchedulerRequestReceiver", "recv_requests", "role", "request_receive"),
    ),
    "sglang.srt.mem_cache.hiradix_cache": (
        ("HiRadixCache", "writing_check", "role", "write_completion_check"),
        ("HiRadixCache", "loading_check", "role", "load_completion_check"),
        ("HiRadixCache", "drain_storage_control_queues", "role", "storage_control_drain"),
        ("HiRadixCache", "can_terminate_prefetch", "role", "prefetch_state_check"),
        ("HiRadixCache", "check_prefetch_progress", "role", "prefetch_completion"),
        ("HiRadixCache", "release_aborted_request", "role", "prefetch_abort"),
        ("HiRadixCache", "query_storage_hit_length", "role", "storage_hit_query"),
    ),
    "sglang.srt.managers.cache_controller": (
        ("HiCacheController", "_all_reduce_prefetch_groups", "role", "prefetch_storage_hit_agreement"),
    ),
}
_CALLS = ContextVar("cpu_collective_calls", default=frozenset())
_CONTEXT = ContextVar("cpu_collective_context", default={})
_RADIX_INSERT = ContextVar("hicache_radix_insert", default=None)
_DEVICE_RELEASE = ContextVar("hicache_device_release", default=None)
# Both public namespaces and compatibility wrappers share each group's order.
_GROUPS = {}


def _record_call(original: Callable[..., Any], event_name: str, fields: dict, *args: Any, **kwargs: Any) -> Any:
    """Record an unchanged call with entry metadata and no return-value inspection."""

    fields["status"] = "raised"
    writer = get_writer()
    start = writer.now_us()
    try:
        result = original(*args, **kwargs)
        fields["status"] = "returned"
        return result
    finally:
        writer.duration_event(event_name, start, writer.now_us(), "runtime_diagnostic", fields)


def _scope(original, field, value):
    def contextual(*args, **kwargs):
        context = dict(_CONTEXT.get(), **{field: value})
        if value in ("prefetch_completion", "prefetch_abort"):
            request_id = kwargs.get(
                "rid" if value == "prefetch_abort" else "req_id", args[1] if len(args) > 1 else None
            )
            if isinstance(request_id, str):
                context["request_id"] = request_id
        token = _CONTEXT.set(context)
        try:
            return original(*args, **kwargs)
        finally:
            _CONTEXT.reset(token)

    return contextual


def _wrap(original, operation, c10d):
    signature = inspect.signature(original)

    def observed(tensor, *args, **kwargs):
        if getattr(getattr(tensor, "device", None), "type", None) != "cpu":
            return original(tensor, *args, **kwargs)
        arguments = signature.bind(tensor, *args, **kwargs)
        arguments.apply_defaults()
        fields = arguments.arguments
        group = fields.get("group")
        if c10d._rank_not_in_group(group) or c10d.get_backend(group) != "gloo":
            return original(tensor, *args, **kwargs)
        group = c10d._get_default_group() if group is None else group
        identity = (group, operation)
        active = _CALLS.get()
        if identity in active:
            return original(tensor, *args, **kwargs)
        sequence = group._get_sequence_number_for_group()
        if group not in _GROUPS:
            _GROUPS[group] = {
                "group": c10d._get_process_group_name(group),
                "members": c10d.get_process_group_ranks(group),
                "rank": c10d.get_rank(),
                "observation_start_sequence": sequence,
                "collective_index": 0,
            }
        record = dict(
            _GROUPS[group],
            operation=operation,
            numel=tensor.numel(),
            dtype=str(tensor.dtype),
            async_op=bool(fields["async_op"]),
            status="raised",
            sequence_before=sequence,
        )
        record.update(_CONTEXT.get())
        # This counts observed calls, including failed/coalesced ones. Gloo's
        # native sequence also counts asymmetric P2P work such as monitored_barrier.
        _GROUPS[group]["collective_index"] += 1
        if operation == "broadcast":
            record.update(src=fields.get("src"), group_src=fields.get("group_src"))
        else:
            record["reduce_op"] = str(fields["op"])
        writer = get_writer()
        start = writer.now_us()
        token = _CALLS.set(active | {identity})
        try:
            result = original(tensor, *args, **kwargs)
            record["status"] = "returned"
            return result
        finally:
            _CALLS.reset(token)
            end = writer.now_us()
            record["sequence_after"] = group._get_sequence_number_for_group()
            writer.duration_event("runtime.cpu_collective", start, end, "runtime_diagnostic", record)

    return observed


def _device_release(original, stage):
    def observed(*args, **kwargs):
        fields = {"status": "raised"}
        writer = get_writer()
        scope = _DEVICE_RELEASE.set(stage)
        start = writer.now_us()
        try:
            result = original(*args, **kwargs)
            # Both SGLang methods already return a Python token count. Do not
            # inspect the node/tensor to recover geometry a second time.
            fields.update(status="returned", released_tokens=result)
            return result
        finally:
            _DEVICE_RELEASE.reset(scope)
            writer.duration_event("runtime.hicache." + stage, start, writer.now_us(), "runtime_diagnostic", fields)

    return observed


def _allocator_free(original):
    def observed(allocator, free_index):
        release = _DEVICE_RELEASE.get()
        if release is None:
            return original(allocator, free_index)
        # numel and allocator flags are host metadata. Never copy indices or
        # synchronize to discover their values. Keep deferred-free distinct.
        fields = dict(
            release=release,
            tokens=free_index.numel(),
            page_size=allocator.page_size,
            immediate=allocator.is_not_in_free_group,
            need_sort=allocator.need_sort,
        )
        return _record_call(original, "runtime.hicache.allocator_free", fields, allocator, free_index)

    return observed


def _node_path_range(node):
    # RadixKey length is Python metadata, not a device tensor operation. Keep
    # positions instead of source node identity: target nodes may split/merge.
    begin = 0
    parent = node.parent
    while parent is not None:
        begin += len(parent.key)
        parent = parent.parent
    return dict(path_begin_tokens=begin, path_end_tokens=begin + len(node.key))


def _radix_insert(original):
    def observed(owner, params):
        fields = dict(chunked=params.chunked, path_tokens=len(params.key), status="raised")
        writer = get_writer()
        start = writer.now_us()
        token = _RADIX_INSERT.set(params.chunked)
        try:
            result = original(owner, params)
            fields["status"] = "returned"
            return result
        finally:
            _RADIX_INSERT.reset(token)
            writer.duration_event("runtime.hicache.radix_insert", start, writer.now_us(), "runtime_diagnostic", fields)

    return observed


def _node_publish(original):
    def observed(owner, node, medium=None):
        chunked = _RADIX_INSERT.get()
        if chunked is None or medium is not None:
            return original(owner, node, medium=medium)
        # The new GPU leaf exists here even when KV event reporting is disabled.
        # Its successful return precedes the optional write-through policy call.
        fields = dict(node_id=node.id, chunked=chunked)
        fields.update(_node_path_range(node))
        return _record_call(original, "runtime.hicache.node_publish", fields, owner, node, medium=medium)

    return observed


def _device_restore(original):
    def observed(owner, node, value, prefix_len):
        chunked = _RADIX_INSERT.get()
        if chunked is None:
            return original(owner, node, value, prefix_len)
        fields = dict(node_id=node.id, chunked=chunked, **_node_path_range(node))
        return _record_call(original, "runtime.hicache.device_restore", fields, owner, node, value, prefix_len)

    return observed


def _write_policy_check(original):
    def observed(owner, node, chunked=False):
        # backuped is exactly host_value is not None in TreeNode. Never inspect
        # its tensor, and retain entry state before write_backup can change it.
        fields = dict(
            node_id=node.id,
            write_policy=owner.cache_controller.write_policy,
            chunked=chunked,
            backuped=node.host_value is not None,
            hit_count=node.hit_count,
            write_through_threshold=owner.write_through_threshold,
        )
        fields.update(_node_path_range(node))
        return _record_call(original, "runtime.hicache.write_policy_check", fields, owner, node, chunked=chunked)

    return observed


def _prefetch_progress(original):
    def observed(owner, req_id, *args, **kwargs):
        # Inspect only entry-state Python references. Completion may remove the
        # operation, so checking after the call would mislabel an active branch.
        pending = getattr(owner, "ongoing_prefetch", None)
        branch = "unknown"
        if pending is not None:
            entry = pending.get(req_id)
            branch = (
                "no_operation"
                if entry is None
                else ("host_not_allocated" if entry[3].host_indices is None else "active")
            )
        fields = {"request_id": req_id, "entry_branch": branch, "status": "raised"}
        writer = get_writer()
        start = writer.now_us()
        try:
            result = original(owner, req_id, *args, **kwargs)
            fields.update(status="returned", progress_ready=result)
            return result
        finally:
            writer.duration_event(
                "runtime.hicache.prefetch_progress", start, writer.now_us(), "runtime_diagnostic", fields
            )

    return observed


def _prefetch_interval(original, stage):
    def observed(owner, operation, *args, **kwargs):
        fields = {"request_id": getattr(operation, "request_id", _CONTEXT.get().get("request_id")), "status": "raised"}
        operation_id = getattr(operation, "id", None)
        if operation_id is not None:
            fields["operation_id"] = operation_id
        if stage == "prefetch_query":
            fields["sync_group_count"] = len(owner.prefetch_sync_groups)
        writer = get_writer()
        start = writer.now_us()
        try:
            result = original(owner, operation, *args, **kwargs)
            fields["status"] = "returned"
            # Existing Python return values only: no tensor reads or cache snapshot.
            if stage == "prefetch_query":
                fields.update(local_hit_tokens=result[1], page_size=owner.page_size)
            elif stage == "prefetch_check":
                fields["can_terminate"] = result
            else:
                fields["completed_tokens"] = result[0]
            return result
        finally:
            writer.duration_event("runtime.hicache." + stage, start, writer.now_us(), "runtime_diagnostic", fields)

    return observed


def _prefetch_enqueue(original):
    def observed(owner, request_id, host_indices, new_input_tokens, *args, **kwargs):
        fields = {
            "request_id": request_id,
            "token_count": len(new_input_tokens),
            "allocated_tokens": host_indices.numel(),
            "page_size": owner.page_size,
            "status": "raised",
        }
        writer = get_writer()
        start = writer.now_us()
        try:
            result = original(owner, request_id, host_indices, new_input_tokens, *args, **kwargs)
            fields.update(status="returned", operation_id=result.id)
            return result
        finally:
            writer.duration_event(
                "runtime.hicache.prefetch_enqueue", start, writer.now_us(), "runtime_diagnostic", fields
            )

    return observed


def _host_release(original):
    def observed(owner, host_indices):
        # Tensor shape metadata only. Attribution needs the same-thread query
        # or worker order; it must never come from inspecting the indices.
        fields = {"token_count": host_indices.numel(), "page_size": owner.mem_pool_host.page_size}
        fields.update(_CONTEXT.get())
        return _record_call(original, "runtime.hicache.host_release", fields, owner, host_indices)

    return observed


def _storage_drain(original):
    def observed(owner, n_revoke, n_backup, n_release, log_metrics):
        # Already reduced Python counts; do not sample queues again or read
        # collective tensors. None means unbounded drain during shutdown.
        fields = {"n_revoke": n_revoke, "n_backup": n_backup, "n_release": n_release}
        fields["tp_world_size"] = owner.tp_world_size
        return _record_call(
            original, "runtime.hicache.storage_drain", fields, owner, n_revoke, n_backup, n_release, log_metrics
        )

    return observed


def _host_load_check(original):
    def observed(req, *args, **kwargs):
        fields = {"request_id": req.rid, "status": "raised"}
        writer = get_writer()
        start = writer.now_us()
        try:
            result = original(req, *args, **kwargs)
            fields.update(status="returned", needed=result)
            return result
        finally:
            writer.duration_event(
                "runtime.hicache.host_load_check", start, writer.now_us(), "runtime_diagnostic", fields
            )

    return observed


def _completion_check(original, stage):
    load = stage == "load_completion"
    queue_name = "ack_load_queue" if load else "ack_write_queue"
    pending_name = "ongoing_load_back" if load else "ongoing_write_through"

    def observed(owner, *args, **kwargs):
        # Scalar lengths only. The main scheduler owns these containers; do not
        # iterate ACK entries, query device events or read collective tensors.
        queue = getattr(owner.cache_controller, queue_name)
        pending = getattr(owner, pending_name)
        fields = {"batches_before": len(queue), "operations_before": len(pending), "status": "raised"}
        fields["blocking"] = False if load else bool(kwargs.get("write_back", args[0] if args else False))
        writer = get_writer()
        start = writer.now_us()
        try:
            result = original(owner, *args, **kwargs)
            fields["status"] = "returned"
            return result
        finally:
            end = writer.now_us()
            fields.update(batches_after=len(queue), operations_after=len(pending))
            writer.duration_event("runtime.hicache." + stage, start, end, "runtime_diagnostic", fields)

    return observed


def _prefetch_publication(original):
    def observed(operation, num_tokens):
        fields = {"request_id": operation.request_id, "num_tokens": num_tokens, "status": "raised"}
        writer = get_writer()
        start = writer.now_us()
        try:
            result = original(operation, num_tokens)
            # Existing scalar state after the real locked increment. A rejected
            # increment still reports the completed prefix; no new lock or tensor read.
            fields.update(status="returned", accepted=result, completed_tokens=operation.completed_tokens)
            return result
        finally:
            writer.duration_event(
                "runtime.hicache.prefetch_publish", start, writer.now_us(), "runtime_diagnostic", fields
            )

    return observed


def _prefetch_read(original):
    def observed(backend, keys, *args, **kwargs):
        # Associate with the enclosing generic read by pid/tid and containment;
        # never inspect file keys, destinations or returned tensors.
        return _record_call(
            original, "runtime.hicache.prefetch_read", {"page_count": len(keys)}, backend, keys, *args, **kwargs
        )

    return observed


_PREFETCH_TIMINGS = {
    "sglang.srt.managers.schedule_batch": (("Req", "needs_host_load_back", _host_load_check),),
    "sglang.srt.mem_cache.hiradix_cache": (
        ("HiRadixCache", "_inc_hit_count", _write_policy_check),
        ("HiRadixCache", "insert", _radix_insert),
        ("HiRadixCache", "_record_store_event", _node_publish),
        ("HiRadixCache", "_restore_device_value", _device_restore),
        ("HiRadixCache", "check_prefetch_progress", _prefetch_progress),
        ("HiRadixCache", "can_terminate_prefetch", partial(_prefetch_interval, stage="prefetch_check")),
        ("HiRadixCache", "_drain_storage_control_queues_impl", _storage_drain),
        ("HiRadixCache", "loading_check", partial(_completion_check, stage="load_completion")),
        ("HiRadixCache", "writing_check", partial(_completion_check, stage="write_completion")),
        ("HiRadixCache", "_evict_backuped", partial(_device_release, stage="device_release_backup")),
        ("HiRadixCache", "_evict_regular", partial(_device_release, stage="device_release_regular")),
    ),
    "sglang.srt.managers.cache_controller": (
        ("HiCacheController", "terminate_prefetch", partial(_prefetch_interval, stage="prefetch_stop")),
        ("HiCacheController", "_storage_hit_query", partial(_prefetch_interval, stage="prefetch_query")),
        ("HiCacheController", "prefetch", _prefetch_enqueue),
        ("HiCacheController", "append_host_mem_release", _host_release),
        ("PrefetchOperation", "increment", _prefetch_publication),
    ),
    "sglang.srt.mem_cache.hicache_storage": (("HiCacheFile", "batch_get", _prefetch_read),),
    "sglang.srt.mem_cache.allocator.paged": (("PagedTokenToKVPoolAllocator", "free", _allocator_free),),
    "sglang.srt.mem_cache.allocator.token": (("TokenToKVPoolAllocator", "free", _allocator_free),),
}
TARGET_MODULES = tuple(
    dict.fromkeys(("torch.distributed.distributed_c10d", "torch.distributed", *_SCOPES, *_PREFETCH_TIMINGS))
)


def install(module: ModuleType) -> None:
    """Install separate context, HiCache timing and CPU collective observations."""
    for class_name, method, field, value in _SCOPES.get(module.__name__, ()):
        owner = getattr(module, class_name, None)
        install_wrapper(owner, method, partial(_scope, field=field, value=value), "__trace_sim_collective_context__")

    for class_name, method, wrapper in _PREFETCH_TIMINGS.get(module.__name__, ()):
        owner = getattr(module, class_name, None)
        install_wrapper(owner, method, wrapper, "__trace_sim_prefetch_timing__")

    if module.__name__ in _SCOPES or module.__name__ in _PREFETCH_TIMINGS:
        return

    c10d = sys.modules.get("torch.distributed.distributed_c10d")
    if c10d is None:
        return

    for name in ("broadcast", "all_reduce"):
        install_wrapper(module, name, partial(_wrap, operation=name, c10d=c10d))
