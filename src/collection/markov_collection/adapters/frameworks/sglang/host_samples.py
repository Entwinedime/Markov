"""Measure native calls on their own CPU threads without graph or device tracing."""

from functools import partial
from statistics import median
from threading import get_native_id, local
from time import thread_time_ns

from markov_adapters.sglang.batches import BatchTensors
from markov_adapters.sglang.clocks import DecisionClocks


class HostCall:
    def __init__(self, target, batch, resumed):
        self.target = target
        self.batch = batch
        self.resumed = resumed
        self.fragments_ns = []
        self.children = []

    def pause(self, now):
        self.fragments_ns.append(now - self.resumed)

    def resume(self):
        self.resumed = thread_time_ns()


class NativeHostSamples:
    batch_target = "sglang.srt.managers.scheduler.Scheduler.run_batch"

    def __init__(self, targets, aliases):
        self.targets = targets
        self.aliases = aliases
        self.enabled = False
        self.threads = local()
        self.intervals = []
        self.batches = []

    @staticmethod
    def clock_cost():
        costs = []
        for _ in range(256):
            begin = thread_time_ns()
            finish = thread_time_ns()
            costs.append(finish - begin)
        return round(median(costs))

    def call(self, target, original, *args, **kwargs):
        if not self.enabled:
            return original(*args, **kwargs)
        parent = getattr(self.threads, "current", None)
        if parent is not None:
            parent.pause(thread_time_ns())
            parent.children.append(target)
        batch = parent.batch if parent is not None else None
        if target == self.batch_target:
            native = args[1] if len(args) > 1 else kwargs["batch"]
            batch = len(self.batches)
            self.batches.append({
                "phase": BatchTensors.phase(native),
                "members": [self.aliases[req.rid] for req in native.reqs],
            })
        current = HostCall(target, batch, 0)
        self.threads.current = current

        # A sampled child pauses its caller around the child and the recorder's bookkeeping.
        current.resumed = thread_time_ns()
        try:
            return original(*args, **kwargs)
        finally:
            current.pause(thread_time_ns())
            self.intervals.append({
                "target": target, "batch": batch, "thread": get_native_id(),
                "children": current.children, "fragments_ns": current.fragments_ns,
            })
            self.threads.current = parent
            if parent is not None:
                parent.resume()

    def clock_read(self, original, description):
        return self.call("time." + description["clock"], original)

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        for target in self.targets:
            if target.startswith("sglang.srt."):
                HookRegistry.register(target, partial(self.call, target), HookType.AROUND)
            elif target not in {"time.perf_counter", "time.monotonic", "time.time"}:
                raise ValueError(f"unsupported native CPU timing target {target}")
        if any(target.startswith("time.") for target in self.targets):
            DecisionClocks(self).install()
        self.timer_ns = self.clock_cost()

    def document(self):
        return {
            "clock": "thread_cpu_ns", "timer_bracket_ns": self.timer_ns,
            "targets": self.targets, "batches": self.batches, "intervals": self.intervals,
        }
