"""Observe native PP tensor transfers and the Work waits that expose their real completion."""

import inspect
from contextlib import contextmanager
from functools import partial

from markov_adapters.sglang.pipeline import PipelineABI


class TransferWork:
    def __init__(self, transfers, native, producer, direction, context) -> None:
        self.transfers = transfers
        self.native = native
        self.producer = producer
        self.direction = direction
        self.context = context

    def wait(self, *args, **kwargs):
        transfers = self.transfers
        if not transfers.calls.enabled:
            return self.native.wait(*args, **kwargs)
        work = transfers.messages.wait_work(self.context, device=False)
        native = type(self.native)
        implementation = f"{native.__module__}.{native.__qualname__}.wait"
        with transfers.consume(f"transfer_{self.direction}_wait", implementation, work, self.producer):
            return self.native.wait(*args, **kwargs)


class TensorTransfers:
    def __init__(self, messages, current, rank_offset) -> None:
        self.messages = messages
        self.calls = messages.calls
        self.current = current
        self.rank_offset = rank_offset

    def invoke(self, signature, original, *args, **kwargs):
        import torch.distributed as distributed

        if not self.calls.enabled:
            return original(*args, **kwargs)
        arguments = signature.bind(*args, **kwargs)
        arguments.apply_defaults()
        values = arguments.arguments
        tensor = values["tensor"]
        if tensor.device.type != "npu":
            return original(*args, **kwargs)
        if self.current.get() != 0:
            raise ValueError("pipeline tensor transfer must expose its own action boundary")
        group = distributed.group.WORLD if values["group"] is None else values["group"]
        receiving = original.__name__ == "irecv"
        endpoint = "src" if receiving else "dst"
        peer = values[endpoint]
        if values.get(f"group_{endpoint}") is not None:
            peer = distributed.get_global_rank(group, values[f"group_{endpoint}"])
        if peer is None:
            raise ValueError("pipeline tensor receive requires an explicit sender")
        rank = distributed.get_rank()
        source, target = (peer, rank) if receiving else (rank, peer)
        offset = self.rank_offset
        members = [offset + source, offset + target]
        direction = "receive" if receiving else "send"
        parameters = PipelineABI.tensor_parameters(tensor)
        work = self.messages.work()
        communication = {
            "channel": f"{group.group_name}/{members[0]}>{members[1]}/{values['tag']}",
            "operation": "transfer", "members": members, "required": members,
            "rank": offset + rank, "order": 0,
        }

        # This boundary sees the actual transmitted slice, after the native send-allgather transformation.
        with self.submit(original, direction, tensor, parameters, work, communication) as producer:
            result = original(*args, **kwargs)
        return TransferWork(self, result, producer, direction, work["context"])

    def install(self):
        import torch.distributed as distributed
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        for name in ("isend", "irecv"):
            HookRegistry.register(
                f"torch.distributed.{name}", partial(self.invoke, inspect.signature(getattr(distributed, name))), HookType.AROUND,
            )


class NativeTransfers(TensorTransfers):
    def __init__(self, observation, messages) -> None:
        super().__init__(messages, observation.capture.current, observation.description["rank_offset"])
        self.observation = observation

    @contextmanager
    def submit(self, original, direction, tensor, parameters, work, communication):
        owner = self.observation
        with owner.capture.call(
            f"transfer_{direction}", original.__qualname__, f"{original.__module__}.{original.__qualname__}",
            parameters=parameters, work=work, device=True,
        ) as call:
            call.record["bindings"]["dtype"] = str(tensor.dtype)
            call.record["asynchronous"] = True
            call.record["communication"] = communication
            if direction == "send":
                call.port("payload", "input", owner.tensors.observe(tensor, list(parameters)), use="device")
            yield call.identity
            if direction == "receive":
                owner.tensors.written(tensor)
                call.port("payload", "output", owner.tensors.observe(tensor, list(parameters)), use="device")
            call.work_signal("completion", "output", call.identity)
        owner.tensors.retire()

    @contextmanager
    def consume(self, role, implementation, work, producer):
        with self.observation.capture.call(role, "Work.wait", implementation, parameters={}, work=work, device=True) as call:
            call.work_signal("completion", "input", producer)
            yield


class HostTransfers(TensorTransfers):
    def __init__(self, host, messages) -> None:
        super().__init__(messages, host.parent, host.rank_offset)

    def submit(self, original, direction, tensor, parameters, work, communication):
        return self.calls.native_call(
            f"transfer_{direction}", original, work=work, parameters=parameters, bindings={"dtype": str(tensor.dtype)},
        )

    def consume(self, role, implementation, work, producer):
        return self.calls.scope(role, "Work.wait", implementation, work=work)
