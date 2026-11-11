"""Hand native CPU protocol intervals to runtime services, outside template CPU work."""

import inspect
from contextlib import contextmanager
from contextvars import ContextVar
from functools import partial

import torch.distributed as distributed

from markov_adapters.pytorch import CpuTransfer


class MetadataWork:
    def __init__(self, owner, native, operation, transfer, group) -> None:
        self.owner, self.native = owner, native
        self.operation, self.transfer, self.group = operation, transfer, group

    def wait(self, *args, **kwargs):
        with self.owner.boundary(self.operation + "_wait", self.transfer, self.group):
            return self.native.wait(*args, **kwargs)


class MetadataTransfers:
    def __init__(self, calls) -> None:
        self.calls = calls
        self.in_transfer = ContextVar("native_metadata_transfer", default=False)

    @contextmanager
    def boundary(self, operation, transfer, group):
        calls = self.calls
        active = calls.protocol_enabled
        parent = calls.current.get()
        work = {}
        if active:
            work = calls.scheduler_work() if parent is None else dict(parent.work)
            transfer = {**transfer, "group": calls.groups.name(group)}
            work["context"] = {**work["context"], "cpu_transfer": CpuTransfer.identity(transfer)}

        # Runtime services own protocol time; the caller must neither overlap it nor expand it as template work.
        with calls.separate("cpu_" + operation, work):
            previous = self.in_transfer.set(True)
            context = calls.communicating.set({"operation": operation, "in_flow": active, "work": work})
            try:
                yield
            finally:
                calls.communicating.reset(context)
                self.in_transfer.reset(previous)

    def invoke(self, name, signature, original, *args, **kwargs):
        if self.in_transfer.get():
            return original(*args, **kwargs)
        arguments = signature.bind(*args, **kwargs)
        arguments.apply_defaults()
        values = arguments.arguments
        tensor = values["tensor"]
        if tensor.device.type != "cpu":
            return original(*args, **kwargs)
        group = distributed.group.WORLD if values["group"] is None else values["group"]
        receiving = name in {"recv", "irecv"}
        endpoint = "src" if receiving else "dst"
        transfer = CpuTransfer(range(distributed.get_world_size())).describe(
            tensor, group, values["tag"], values[endpoint], values.get("group_" + endpoint), receiving,
        )
        with self.boundary(name, transfer, group):
            result = original(*args, **kwargs)
        return MetadataWork(self, result, name, transfer, group) if name in {"isend", "irecv"} else result

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        for name in ("send", "recv", "isend", "irecv"):
            hook = partial(self.invoke, name, inspect.signature(getattr(distributed, name)))
            for module in ("torch.distributed", "torch.distributed.distributed_c10d"):
                HookRegistry.register(f"{module}.{name}", hook, HookType.AROUND)
