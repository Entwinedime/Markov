"""Observe native PP tensor transfers and the Work waits that expose their real completion."""

import inspect
from functools import partial

from markov_adapters.sglang.pipeline import PipelineABI


class TransferWork:
    def __init__(self, observation, messages, work, producer, direction, context) -> None:
        self.observation = observation
        self.messages = messages
        self.work = work
        self.producer = producer
        self.direction = direction
        self.context = context

    def wait(self, *args, **kwargs):
        owner = self.observation
        if not owner.active:
            return self.work.wait(*args, **kwargs)
        work = self.messages.wait_work(self.context, device=False)
        with owner.capture.call(
            f"transfer_{self.direction}_wait", "Work.wait", f"{type(self.work).__module__}.{type(self.work).__qualname__}.wait",
            parameters={}, work=work, device=True,
        ) as call:
            call.work_signal("completion", "input", self.producer)
            result = self.work.wait(*args, **kwargs)
        owner.capture.runtime.retire()
        return result


class NativeTransfers:
    def __init__(self, observation, messages) -> None:
        self.observation = observation
        self.messages = messages

    def invoke(self, signature, original, *args, **kwargs):
        import torch.distributed as distributed

        owner = self.observation
        arguments = signature.bind(*args, **kwargs)
        arguments.apply_defaults()
        values = arguments.arguments
        tensor = values["tensor"]
        if not owner.active or not owner.selected or tensor.device.type != "npu":
            return original(*args, **kwargs)
        if owner.capture.current.get() != 0:
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
        offset = owner.description["rank_offset"]
        members = [offset + source, offset + target]
        direction = "receive" if receiving else "send"
        parameters = PipelineABI.tensor_parameters(tensor)
        work = self.messages.work()

        # This boundary sees the actual transmitted slice, after the native send-allgather transformation.
        with owner.capture.call(
            f"transfer_{direction}", original.__qualname__, f"{original.__module__}.{original.__qualname__}",
            parameters=parameters, work=work, device=True,
        ) as call:
            call.record["bindings"]["dtype"] = str(tensor.dtype)
            call.record["asynchronous"] = True
            call.record["communication"] = {
                "channel": f"{group.group_name}/{members[0]}>{members[1]}/{values['tag']}",
                "operation": "transfer", "members": members, "required": members,
                "rank": offset + rank, "order": 0,
            }
            if not receiving:
                call.port("payload", "input", owner.tensors.observe(tensor, list(parameters)), use="device")
            result = original(*args, **kwargs)
            if receiving:
                owner.tensors.written(tensor)
                call.port("payload", "output", owner.tensors.observe(tensor, list(parameters)), use="device")
            call.work_signal("completion", "output", call.identity)
        owner.tensors.retire()
        owner.capture.runtime.retire()
        return TransferWork(owner, self.messages, result, call.identity, direction, work["context"])

    def install(self):
        import torch.distributed as distributed
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        for name in ("isend", "irecv"):
            HookRegistry.register(
                f"torch.distributed.{name}", partial(self.invoke, inspect.signature(getattr(distributed, name))), HookType.AROUND,
            )
