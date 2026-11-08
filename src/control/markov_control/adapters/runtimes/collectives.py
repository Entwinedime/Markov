"""Transport CPU metadata; framework action boundaries own simulation time."""

from dataclasses import dataclass, field

import torch
import torch.distributed as distributed

from markov_control.adapters.runtimes.metadata import MetadataWork


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

    def exchange(self, kind, tensor, output, group, async_op, **parameters):
        if tensor.device.type != "cpu" or output.device.type != "cpu":
            raise ValueError("Native metadata collectives require CPU tensors")

        # Gloo returns locally for an empty reduction; posting it would introduce a peer rendezvous.
        handle = None
        if kind != "minimum" or tensor.numel() != 0:
            group = distributed.group.WORLD if group is None else group
            members = tuple(self.world[rank] for rank in distributed.get_process_group_ranks(group))
            handle = self.peer.exchange(
                "collective_post",
                {
                    "group": group.group_name, "members": members, "kind": kind,
                    "dtype": str(tensor.dtype), "values": tensor.flatten().tolist(), **parameters,
                },
            )
        work = MetadataWork(self.peer, handle, output, "collective_wait")
        if async_op:
            return work
        work.wait()

    def broadcast(self, tensor, src=None, group=None, async_op=False, group_src=None):
        if group_src is not None:
            src = distributed.get_process_group_ranks(group)[group_src]
        return self.exchange("broadcast", tensor, tensor, group, async_op, source=self.world[src])

    def all_gather_into_tensor(self, output_tensor, input_tensor, group=None, async_op=False):
        return self.exchange("gather", input_tensor, output_tensor, group, async_op)

    def all_reduce(self, tensor, op=distributed.ReduceOp.SUM, group=None, async_op=False):
        if op != distributed.ReduceOp.MIN:
            raise ValueError("Native metadata polling requires a MIN reduction")
        return self.exchange("minimum", tensor, tensor, group, async_op)


@dataclass
class CollectiveCall:
    header: dict
    values: dict = field(default_factory=dict)
    waiting: dict = field(default_factory=dict)
    consumed: set = field(default_factory=set)
    result: list | None = None


class CollectiveCalls:
    def __init__(self) -> None:
        self.pending = {}
        self.positions = {}

    def post(self, channel, rank, message) -> None:
        members = message["members"]
        if rank not in members:
            raise ValueError("Native CPU collective has an invalid participant")

        # Participants can enter the next broadcast while another receiver is still in the preceding call.
        group = members, message["group"]
        participant = group, rank
        position = self.positions.get(participant, 0)
        key = (*group, position)
        header = {name: value for name, value in message.items() if name != "values"}
        call = self.pending.setdefault(key, CollectiveCall(header))
        if call.header != header:
            raise ValueError("Native CPU collective participants disagree on the operation")
        if call.values and len(next(iter(call.values.values()))) != len(message["values"]):
            raise ValueError("Native CPU collective participants disagree on tensor extent")
        self.positions[participant] = position + 1
        call.values[rank] = message["values"]
        kind = message["kind"]
        if kind == "broadcast":
            source = message["source"]
            if source not in members:
                raise ValueError("Native CPU broadcast source is not a group member")

            # A root can enter another broadcast while late receivers still need this payload.
            if source in call.values:
                call.result = call.values[source]
        elif len(call.values) == len(members):
            if kind == "gather":
                call.result = [value for member in members for value in call.values[member]]
            elif kind == "minimum":
                dtype = getattr(torch, message["dtype"].removeprefix("torch."))
                tensors = torch.tensor([call.values[member] for member in members], dtype=dtype)
                call.result = tensors.amin(dim=0).tolist()
            else:
                raise ValueError(f"Unknown native metadata collective: {kind}")

        # Acknowledge submission independently of result readiness, so every actor can submit its graph.
        channel.send(key)
        self.complete(key, call)

    def wait(self, channel, rank, handle) -> None:
        key = tuple(handle)
        call = self.pending[key]
        if rank not in call.values or rank in call.consumed:
            raise ValueError("Native CPU collective wait has no outstanding local operation")
        call.waiting[rank] = channel
        self.complete(key, call)

    def complete(self, key, call) -> None:
        if call.result is None:
            return
        for rank, channel in call.waiting.items():
            channel.send(call.result)
            call.consumed.add(rank)
        call.waiting.clear()

        # Completion of one actor never reclaims data still owned by another actor's native Work.
        if len(call.consumed) == len(call.header["members"]):
            del self.pending[key]

    def blocked(self):
        return {channel for call in self.pending.values() for channel in call.waiting.values()}
