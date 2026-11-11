"""Keep CPU protocol execution between the same caller fragments in capture and control."""

from contextlib import contextmanager
from functools import partial


class MetadataWork:
    def __init__(self, owner, native, operation) -> None:
        self.owner, self.native, self.operation = owner, native, operation

    def wait(self, *args, **kwargs):
        with self.owner.boundary(self.operation + "_wait"):
            return self.native.wait(*args, **kwargs)


class MetadataTransfers:
    def __init__(self, calls) -> None:
        self.calls = calls

    @contextmanager
    def boundary(self, operation):
        parent = self.calls.current.get()
        if not self.calls.enabled or parent is None:
            yield
            return

        # Native metadata owns this interval; the caller's preceding work must already be submitted.
        with self.calls.separate("cpu_" + operation, dict(parent.work)):
            yield

    def invoke(self, name, original, *args, **kwargs):
        tensor = args[0] if args else kwargs["tensor"]
        if tensor.device.type != "cpu":
            return original(*args, **kwargs)
        with self.boundary(name):
            result = original(*args, **kwargs)
        return MetadataWork(self, result, name) if name in {"isend", "irecv"} else result

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        for name in ("send", "recv", "isend", "irecv"):
            HookRegistry.register(f"torch.distributed.{name}", partial(self.invoke, name), HookType.AROUND)
