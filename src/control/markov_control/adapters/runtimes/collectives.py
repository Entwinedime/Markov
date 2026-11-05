"""Expose native CPU metadata waits to the worker broker without advancing simulation time."""

from dataclasses import dataclass, field

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

        # Gloo returns locally for an empty reduction; waiting for peers would introduce a barrier.
        if kind == "minimum" and tensor.numel() == 0:
            return []
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


@dataclass
class CollectiveCall:
    header: dict
    values: dict = field(default_factory=dict)
    waiting: dict = field(default_factory=dict)


class CollectiveCalls:
    def __init__(self) -> None:
        self.pending = {}
        self.positions = {}

    def submit(self, channel, rank, message) -> None:
        members = message["members"]
        if rank not in members:
            raise ValueError("Native CPU collective has an invalid participant")

        # Participants can enter the next broadcast while another receiver is still in the preceding call.
        group = members, message["group"]
        participant = group, rank
        position = self.positions.get(participant, 0)
        key = group, position
        header = {name: value for name, value in message.items() if name != "values"}
        call = self.pending.setdefault(key, CollectiveCall(header))
        if call.header != header:
            raise ValueError("Native CPU collective participants disagree on the operation")
        if call.values and len(next(iter(call.values.values()))) != len(message["values"]):
            raise ValueError("Native CPU collective participants disagree on tensor extent")
        self.positions[participant] = position + 1
        call.values[rank] = message["values"]
        call.waiting[rank] = channel

        kind = message["kind"]
        if kind == "broadcast":
            source = message["source"]
            if source not in members:
                raise ValueError("Native CPU broadcast source is not a group member")
            if source not in call.values:
                return

            # Retaining the source payload permits local returns; broadcast is not an all-rank barrier.
            values = call.values[source]
        else:
            if len(call.values) != len(members):
                return
            if kind == "gather":
                values = [value for member in members for value in call.values[member]]
            elif kind == "minimum":
                dtype = getattr(torch, message["dtype"].removeprefix("torch."))
                tensors = torch.tensor([call.values[member] for member in members], dtype=dtype)
                values = tensors.amin(dim=0).tolist()
            else:
                raise ValueError(f"Unknown native metadata collective: {kind}")
        for participant in call.waiting.values():
            participant.send(values)
        call.waiting.clear()
        if len(call.values) == len(members):
            del self.pending[key]

    def blocked(self):
        return {channel for call in self.pending.values() for channel in call.waiting.values()}
