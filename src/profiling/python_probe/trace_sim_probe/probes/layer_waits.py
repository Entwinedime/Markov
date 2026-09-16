"""Batch-buffered HiCache layer-wait boundaries, without per-wait serialization."""

import functools
import sys
import time
from contextvars import ContextVar

from trace_sim_probe.patching import PATCH_MARKER
from trace_sim_probe.writer import get_writer

_BATCH = ContextVar("hicache_layer_waits", default=None)
TARGET_MODULES = ("sglang.srt.managers.cache_controller", "sglang.srt.managers.tp_worker")


def _wait(original):
    @functools.wraps(original)
    def observed(counter, threshold):
        current = _BATCH.get()
        if current is None or current[0] is not counter:
            return original(counter, threshold)
        clock = current[2]
        start = clock()
        result = original(counter, threshold)
        end = clock()
        current[1].append((threshold, start, end))
        return result
    return observed


def _forward(original):
    @functools.wraps(original)
    def observed(worker, batch, *args, **kwargs):
        counter = getattr(worker, "hicache_layer_transfer_counter", None)
        if batch is None or counter is None:
            return original(worker, batch, *args, **kwargs)
        intervals = []
        # Use the same counter as Torch NPU; its wall-clock conversion is only
        # available after export. Do not import or initialize a device here.
        profiler = sys.modules.get("torch_npu._C._profiler")
        clock = getattr(profiler, "_get_syscnt", None)
        fields = {"request_ids": [req.rid for req in batch.reqs], "phase": batch.forward_mode.name,
                  "consumer_index": batch.hicache_consumer_index, "layer_count": counter.num_layers,
                  "wait_intervals": intervals, "wait_clock": "npu_syscnt" if clock else "unix_ns", "status": "raised"}
        writer = get_writer()
        token = _BATCH.set((counter, intervals, clock or time.time_ns))
        start = writer.now_us()
        try:
            result = original(worker, batch, *args, **kwargs)
            fields["status"] = "returned"
            return result
        finally:
            end = writer.now_us()
            _BATCH.reset(token)
            writer.duration_event("runtime.hicache.layer_waits", start, end, "runtime_diagnostic", fields)
    return observed


def install(module):
    if module.__name__ == TARGET_MODULES[0]:
        class_name, method, wrapper = "LayerDoneCounter", "wait_until", _wait
    else:
        class_name, method, wrapper = "TpModelWorker", "forward_batch_generation", _forward
    owner = getattr(module, class_name, None)
    original = getattr(owner, method, None)
    if original is not None and not getattr(original, PATCH_MARKER, False):
        measured = wrapper(original)
        setattr(measured, PATCH_MARKER, True)
        setattr(owner, method, measured)
