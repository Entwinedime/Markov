"""Time the native scope hierarchy without recording tensor ports or device markers."""

from contextlib import contextmanager
from contextvars import ContextVar
from functools import partial, update_wrapper
from threading import get_native_id
from time import CLOCK_MONOTONIC_RAW, clock_gettime_ns

from markov_adapters.sglang.state_calls import RequestStateCalls
from markov_collection.adapters.frameworks.sglang.flow_scopes import ServingCuts
from markov_collection.adapters.frameworks.sglang.module_scopes import ModelScopes
from markov_collection.adapters.runtimes.ascend.host_profile import AscendHostProfile


class NativeHostSamples(ServingCuts):
    def __init__(self, aliases, rank, directory):
        super().__init__(aliases)
        self.rank = rank
        self.enabled = False
        self.request_state = RequestStateCalls(observe=False)
        self.parent = ContextVar("timed_host_scope", default=0)
        self.scopes = [{"id": 0, "parent": None, "role": "window", "site": "", "implementation": "", "work": {}, "bindings": {}}]
        self.auxiliary_intervals = []
        self.profile = AscendHostProfile(directory)

    @contextmanager
    def scope(self, role, site, implementation, *, work=None, condition=None):
        occurrence = {
            "id": len(self.scopes), "parent": self.parent.get(), "role": role, "site": site,
            "implementation": implementation, "work": {} if work is None else dict(work),
            "bindings": {} if condition is None else {"condition": condition}, "thread": get_native_id(),
        }
        self.scopes.append(occurrence)
        previous = self.parent.set(occurrence["id"])
        begin = clock_gettime_ns(CLOCK_MONOTONIC_RAW)
        try:
            yield occurrence
        finally:
            occurrence["raw_interval"] = [begin, clock_gettime_ns(CLOCK_MONOTONIC_RAW)]
            self.parent.reset(previous)

    def native_call(self, role, original, **facts):
        return self.scope(role, original.__qualname__, f"{original.__module__}.{original.__qualname__}", **facts)

    def enclosing(self, frame):
        return self.native_call("invocation", frame.original, work=frame.work)

    @contextmanager
    def fragment(self, frame):
        with self.native_call("segment", frame.original, condition=frame.condition) as occurrence:
            yield
        occurrence["work"]["continuation"] = frame.continuation

    @contextmanager
    def boundary(self, original, boundary):
        with self.native_call(boundary.operation, original, work=boundary.work) as occurrence:
            yield
        occurrence["work"]["context"] = boundary.work["context"]

    @contextmanager
    def read_boundary(self, kind, original, work, condition):
        with self.native_call("query", original, work=work, condition=condition):
            yield {}

    def load_model(self, original, runner):
        result = original(runner)
        modules = dict(runner.model.named_modules())
        for site, role in ModelScopes.selected(modules).items():
            module = modules[site]
            implementation = f"{type(module).__module__}.{type(module).__qualname__}"
            forward = module.forward
            module.forward = update_wrapper(partial(self.module, role, site, implementation, forward), forward)
        return result

    def module(self, role, site, implementation, original, *args, **kwargs):
        if not self.enabled:
            return original(*args, **kwargs)
        with self.scope(role, site, implementation):
            return original(*args, **kwargs)

    def run_batch(self, original, scheduler, batch, *args, **kwargs):
        if not self.enabled:
            return original(scheduler, batch, *args, **kwargs)
        with self.native_call("run_batch", original, work=self.batch_work(batch)):
            return original(scheduler, batch, *args, **kwargs)

    def forward(self, original, runner, *args, **kwargs):
        if not self.enabled:
            return original(runner, *args, **kwargs)
        with self.native_call("model", original):
            return original(runner, *args, **kwargs)

    def sample(self, original, runner, *args, **kwargs):
        if not self.enabled:
            return original(runner, *args, **kwargs)
        with self.native_call("sample", original):
            return original(runner, *args, **kwargs)

    @contextmanager
    def auxiliary(self):
        if not self.enabled:
            yield
            return
        begin = clock_gettime_ns(CLOCK_MONOTONIC_RAW)
        try:
            yield
        finally:
            self.auxiliary_intervals.append([get_native_id(), begin, clock_gettime_ns(CLOCK_MONOTONIC_RAW)])

    def install(self):
        super().install()
        ModelScopes.install_engine_hooks(self)

    def document(self):
        return {
            "clock": "monotonic_raw_ns", "scopes": self.scopes,
            "auxiliary_intervals": self.auxiliary_intervals, "timing": self.profile.finish(),
        }
