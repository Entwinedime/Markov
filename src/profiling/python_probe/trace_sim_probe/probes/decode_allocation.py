"""Observe decode/load allocation and capacity boundaries without tensor reads."""

from types import ModuleType

from trace_sim_probe.patching import install_wrapper
from trace_sim_probe.probes.hicache.common import _cache_scope_key
from trace_sim_probe.writer import get_writer

TARGET_MODULES = (
    "sglang.srt.mem_cache.common",
    "sglang.srt.managers.schedule_batch",
    "sglang.srt.managers.cache_controller",
)
_MARKER = "__trace_sim_decode_allocation__"
_CAPACITY_MARKER = "__trace_sim_capacity_guard__"
_LOAD_MARKER = "__trace_sim_load_allocation__"


def _load_attempt(original):
    def observed(controller, *args, **kwargs):
        # A successful return queues work; it is not device completion. Merely
        # test object identity against None: never inspect the returned tensor.
        fields = {"node_id": kwargs.get("node_id", args[2] if len(args) > 2 else -1), "status": "raised"}
        writer = get_writer()
        start = writer.now_us()
        try:
            result = original(controller, *args, **kwargs)
            fields.update(status="returned", allocated=result is not None)
            return result
        finally:
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
        fields = {
            "cache_scope": _cache_scope_key(batch.tree_cache),
            "token_per_req": token_per_req,
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
    if module.__name__ == "sglang.srt.managers.cache_controller":
        controller = getattr(module, "HiCacheController", None)
        install_wrapper(controller, "load", _load_attempt, _LOAD_MARKER)
        return

    if module.__name__ not in TARGET_MODULES:
        return
    install_wrapper(module, "alloc_for_decode", _allocation, _MARKER)

    if module.__name__ == "sglang.srt.mem_cache.common":
        install_wrapper(module, "evict_from_tree_cache", _capacity_guard, _CAPACITY_MARKER)
