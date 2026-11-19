"""Time the native scope hierarchy without recording tensor ports or device markers."""

from contextlib import contextmanager
from contextvars import ContextVar
from functools import partial, update_wrapper
from threading import get_native_id
from time import CLOCK_MONOTONIC_RAW, clock_gettime_ns

from markov_adapters.sglang.groups import ParallelGroups
from markov_adapters.sglang.metadata_transfers import MetadataTransfers
from markov_adapters.sglang.pipeline import PipelineABI
from markov_adapters.sglang.state_calls import RequestStateCalls
from markov_collection.adapters.frameworks.sglang.communications import DeviceCollectiveCuts
from markov_collection.adapters.frameworks.sglang.flow_scopes import ServingCuts
from markov_collection.adapters.frameworks.sglang.kv_scopes import NativeKvScopes
from markov_collection.adapters.frameworks.sglang.module_scopes import ModelScopes
from markov_collection.adapters.frameworks.sglang.pipeline_scopes import HostPipelineScopes
from markov_collection.adapters.runtimes.ascend.host_profile import AscendHostProfile
from markov_collection.adapters.runtimes.pytorch.protocol import CpuProtocol


class NativeHostSamples(ServingCuts):
    def __init__(self, aliases, description):
        super().__init__(aliases, description["mode"])
        self.rank_offset = description["rank_offset"]
        self.ep_size = description["ep_size"]
        self.enabled = False
        self.request_state = RequestStateCalls(observe=False)
        self.parent = ContextVar("timed_host_scope", default=0)
        self.scopes = [{"id": 0, "parent": None, "role": "window", "site": "", "implementation": "", "work": {}, "bindings": {}}]
        self.auxiliary_intervals = []
        self.profile = None
        self.communications = DeviceCollectiveCuts(self)
        self.protocol = None
        self.pipeline = None
        if description["pp_size"] > 1:
            self.protocol = CpuProtocol(
                description["native_participants"], self.communicating, self.parent, self.protocol_service,
            )
            self.pipeline = HostPipelineScopes(self)

    def start(self, scheduler, rank, directory):
        self.scheduler = scheduler
        self.rank = rank
        self.groups = ParallelGroups()
        self.collective_groups = self.groups.document(self.rank_offset)
        self.profile = AscendHostProfile(directory)
        self.profile.start()
        if self.protocol is not None:
            self.protocol.begin()

    @staticmethod
    @contextmanager
    def protocol_service(interval):
        interval["begin_raw_ns"] = clock_gettime_ns(CLOCK_MONOTONIC_RAW)
        try:
            yield
        finally:
            interval["end_raw_ns"] = clock_gettime_ns(CLOCK_MONOTONIC_RAW)

    @contextmanager
    def scope(self, role, site, implementation, *, work=None, condition=None, parameters=None, bindings=None):
        occurrence = {
            "id": len(self.scopes), "parent": self.parent.get(), "role": role, "site": site,
            "implementation": implementation, "work": {} if work is None else dict(work),
            "bindings": {} if bindings is None else dict(bindings), "thread": get_native_id(),
        }
        if condition is not None:
            occurrence["bindings"]["condition"] = condition
        if parameters is not None:
            occurrence["parameters"] = parameters
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

    @property
    def collective_at_root(self):
        return self.parent.get() == 0

    def observe_exchange(self, original, role, group, root_rank, gather, run):
        work, parameters, bindings = None, None, None
        if role == "gather":
            _, tensor, dimension = gather
            parameters = PipelineABI.tensor_parameters(tensor)
            group_role = self.groups.name(group)
            bindings = {"collective": PipelineABI.gather_condition(tensor, dimension, group_role)}
            if self.pipeline is not None:
                work = self.pipeline.messages.work()
        with self.native_call(role, original, work=work, parameters=parameters, bindings=bindings):
            return run()

    def observe_expert_exchange(self, original, dispatcher, *args, **kwargs):
        if not self.enabled:
            return original(dispatcher, *args, **kwargs)
        with self.native_call("communication", original):
            return original(dispatcher, *args, **kwargs)

    def collective(self, original, boundary, arguments):
        role = boundary.condition["collective"]["group"]
        with self.native_call(boundary.operation, original, work=boundary.work, condition=boundary.condition) as scope:
            scope["group"] = self.collective_groups[role]
            scope["parameters"] = boundary.parameters
            return original(**arguments)

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
        occurrence["parameters"] = boundary.parameters

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

    def observe_expert_layers(self, experts):
        for site, module in experts.items():
            implementation = f"{type(module).__module__}.{type(module).__qualname__}"
            forward = module.forward
            module.forward = update_wrapper(partial(self.module, "experts", site, implementation, forward), forward)
            compute = module.run_moe_core
            module.run_moe_core = update_wrapper(
                partial(self.module, "expert_compute", compute.__qualname__, f"{compute.__module__}.{compute.__qualname__}", compute),
                compute,
            )

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
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        if self.protocol is not None:
            for target, hook in self.protocol.hooks():
                HookRegistry.register(target, hook, HookType.AROUND)
        ModelScopes.install_engine_hooks(self)
        self.communications.install(self.observe_expert_exchange if self.ep_size > 1 else None)
        if self.mode != "null":
            NativeKvScopes(self).install()
        super().install()
        if self.pipeline is not None:
            self.pipeline.install()
            MetadataTransfers(self).install()

    def document(self):
        document = {
            "clock": "monotonic_raw_ns", "scopes": self.scopes,
            "auxiliary_intervals": self.auxiliary_intervals, "timing": self.profile.finish(),
        }
        if self.protocol is not None:
            document["protocol"] = self.protocol.document()
        return document
