"""Expose native CPU metadata waits to the worker broker without advancing simulation time."""

import torch
import torch.distributed as distributed


class CpuCollectives:
    def __init__(self, peer, world) -> None:
        self.peer = peer
        self.world = world

    @classmethod
    def install(cls, peer, world) -> None:
        bridge = cls(peer, world)
        for name in ("broadcast", "all_gather_into_tensor", "all_reduce"):
            operation = getattr(bridge, name)
            setattr(distributed, name, operation)
            setattr(distributed.distributed_c10d, name, operation)

    def exchange(self, kind, tensor, group, async_op, **parameters):
        if tensor.device.type != "cpu" or async_op:
            raise ValueError("Native metadata collectives require blocking CPU tensors")
        group = distributed.group.WORLD if group is None else group
        members = tuple(self.world[rank] for rank in distributed.get_process_group_ranks(group))
        return self.peer.exchange(
            "collective",
            {
                "group": group.group_name,
                "members": members,
                "kind": kind,
                "dtype": str(tensor.dtype),
                "values": tensor.flatten().tolist(),
                **parameters,
            },
        )

    def broadcast(self, tensor, src=None, group=None, async_op=False, group_src=None):
        if group_src is not None:
            src = distributed.get_process_group_ranks(group)[group_src]
        values = self.exchange("broadcast", tensor, group, async_op, source=self.world[src])
        tensor.copy_(torch.tensor(values, dtype=tensor.dtype).reshape_as(tensor))

    def all_gather_into_tensor(self, output_tensor, input_tensor, group=None, async_op=False):
        values = self.exchange("gather", input_tensor, group, async_op)
        output_tensor.copy_(torch.tensor(values, dtype=output_tensor.dtype).reshape_as(output_tensor))

    def all_reduce(self, tensor, op=distributed.ReduceOp.SUM, group=None, async_op=False):
        if op != distributed.ReduceOp.MIN:
            raise ValueError("Native metadata polling requires a MIN reduction")
        values = self.exchange("minimum", tensor, group, async_op)
        tensor.copy_(torch.tensor(values, dtype=tensor.dtype).reshape_as(tensor))


class CollectiveCalls:
    def __init__(self) -> None:
        self.pending = {}

    def submit(self, channel, rank, message) -> None:
        members = message["members"]
        key = members, message["group"]
        participants = self.pending.setdefault(key, {})
        if rank not in members or rank in participants:
            raise ValueError("Native CPU collective has an invalid or repeated participant")
        participants[rank] = channel, message
        if len(participants) != len(members):
            return

        # A process-group identity separates independent collectives with the same member ranks.
        header = {name: value for name, value in message.items() if name != "values"}
        calls = [participants[member] for member in members]
        for _, call in calls:
            if {name: value for name, value in call.items() if name != "values"} != header:
                raise ValueError("Native CPU collective participants disagree on the operation")
            if len(call["values"]) != len(message["values"]):
                raise ValueError("Native CPU collective participants disagree on tensor extent")

        kind = message["kind"]
        if kind == "broadcast":
            values = participants[message["source"]][1]["values"]
        elif kind == "gather":
            values = [value for _, call in calls for value in call["values"]]
        elif kind == "minimum":
            dtype = getattr(torch, message["dtype"].removeprefix("torch."))
            tensors = torch.tensor([call["values"] for _, call in calls], dtype=dtype)
            values = tensors.amin(dim=0).tolist()
        else:
            raise ValueError(f"Unknown native metadata collective: {kind}")
        for participant, _ in calls:
            participant.send(values)
        del self.pending[key]

    def blocked(self):
        return {channel for calls in self.pending.values() for channel, _ in calls.values()}
