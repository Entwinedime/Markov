"""Intercept decision clocks in the native ABI without changing Python's process clock."""

import importlib
import time
from contextvars import ContextVar
from functools import partial


class ClockNamespace:
    def __init__(self, owner, native, module) -> None:
        self.owner, self.native, self.module = owner, native, module

    def __getattr__(self, name):
        consumer = self.owner.consumer.get()
        if consumer is not None and consumer.startswith(self.module + ".") and name in {"time", "monotonic", "perf_counter"}:
            return partial(self.owner.read, name, consumer)
        return getattr(self.native, name)


class DecisionClocks:
    receiver_publication = "sglang.srt.disaggregation.mooncake.conn.MooncakeKVReceiver.send_metadata"

    def __init__(self, calls) -> None:
        self.calls = calls
        self.consumer = ContextVar("native_decision_clock", default=None)

    def read(self, name, consumer):
        return self.calls.clock_read(getattr(time, name), {"kind": "clock", "clock": name, "consumer": consumer})

    def invoke(self, consumer, original, *args, **kwargs):
        previous = self.consumer.set(consumer)
        try:
            return original(*args, **kwargs)
        finally:
            self.consumer.reset(previous)

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        # These functions create or consume policy timestamps; metrics and background/watchdog clocks stay native.
        consumers = {
            "managers.scheduler": (
                "Scheduler._abort_on_running_timeout", "Scheduler._abort_on_waiting_timeout",
            ),
            "observability.req_time_stats": (
                "SchedulerReqTimeStats.set_wait_queue_entry_time", "SchedulerReqTimeStats.set_forward_entry_time",
            ),
            "disaggregation.decode": ("DecodePreallocQueue._ensure_prefill_info",),
            "disaggregation.common.conn": (
                "CommonKVSender._check_bootstrap_timeout", "CommonKVReceiver._check_waiting_timeout",
            ),
            "disaggregation.mooncake.conn": (
                "MooncakeKVSender.__init__", "MooncakeKVReceiver.send_metadata",
            ),
        }
        for name, methods in consumers.items():
            module = importlib.import_module("sglang.srt." + name)
            module.time = ClockNamespace(self, module.time, module.__name__)
            for method in methods:
                target = module.__name__ + "." + method
                HookRegistry.register(target, partial(self.invoke, target), HookType.AROUND)
