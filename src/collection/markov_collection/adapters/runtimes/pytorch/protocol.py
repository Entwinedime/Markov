"""Observe Gloo post and Work-consumption facts without inferring messages from profiler names."""

import inspect
import threading
from contextvars import ContextVar
from functools import partial

from markov_adapters.pytorch import CpuTransfer


class ProtocolWork:
    def __init__(self, observer, native, post) -> None:
        self.observer, self.native, self.post = observer, native, post
        self.consumed = False

    def wait(self, *args, **kwargs):
        if self.consumed:
            return self.native.wait(*args, **kwargs)
        interval = {"thread": threading.get_native_id()}
        with self.observer.runtime.service_scope(interval):
            result = self.native.wait(*args, **kwargs)
        if result:
            self.consumed = True
            self.observer.publish(self.post, interval)
        return result


class CpuProtocol:
    def __init__(self, runtime, world) -> None:
        self.runtime = runtime
        self.transfer = CpuTransfer(world)
        self.lock = threading.Lock()
        self.positions = {}
        self.posts = {}
        self.active = False
        self.enclosing = ContextVar("cpu_protocol", default=False)

    def begin(self):
        with self.lock:
            self.start = list(self.positions.items())
            self.posts = {}
            self.active = True

    def publish(self, post, consumed=None):
        with self.lock:
            if self.active:
                recorded = self.posts.setdefault(post["key"], {**post, "consumed": None})
                if consumed is not None:
                    recorded["consumed"] = consumed

    def invoke(self, name, signature, original, *args, **kwargs):
        if self.enclosing.get():
            return original(*args, **kwargs)
        import torch.distributed as distributed

        arguments = signature.bind(*args, **kwargs)
        arguments.apply_defaults()
        values = arguments.arguments
        tensor = values["tensor"]
        group = distributed.group.WORLD if values["group"] is None else values["group"]
        if tensor.device.type != "cpu" or distributed.get_backend(group) != "gloo":
            return original(*args, **kwargs)
        receiving = name in {"recv", "irecv"}
        endpoint = "src" if receiving else "dst"
        description = self.transfer.describe(tensor, group, values["tag"], values[endpoint], values.get("group_" + endpoint), receiving)
        route = (description["group"], description["source"], description["target"], description["tag"])
        with self.lock:
            position = self.positions.get(route, 0)
            self.positions[route] = position + 1
        post = {"key": (*route, position), "descriptor": description, "interval": {"thread": threading.get_native_id()}}

        # Keep preposted Work facts on its handle; a wait can cross the observation's beginning.
        previous = self.enclosing.set(True)
        try:

            # Blocking PyTorch send/recv delegate to isend/irecv; retain one actual protocol operation.
            with self.runtime.service_scope(post["interval"]):
                result = original(*args, **kwargs)
        finally:
            self.enclosing.reset(previous)
        asynchronous = name in {"isend", "irecv"}
        self.publish(post, None if asynchronous else post["interval"])
        return ProtocolWork(self, result, post) if asynchronous else result

    def hooks(self):
        import torch.distributed as distributed

        return [(f"torch.distributed.{name}", partial(self.invoke, name, inspect.signature(getattr(distributed, name))))
                for name in ("send", "recv", "isend", "irecv")]

    def document(self):
        with self.lock:
            self.active = False
            return {
                "backend": "gloo", "operations": ["send", "recv", "isend", "irecv"],
                "start_positions": [{"route": route, "position": position} for route, position in self.start],
                "end_positions": [{"route": route, "position": position} for route, position in self.positions.items()],
                "posts": list(self.posts.values()),
            }
