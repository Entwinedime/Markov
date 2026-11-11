"""Expose native CPU metadata sends and receives without hiding blocked workers inside Gloo."""

from dataclasses import dataclass, field

import torch.distributed as distributed

from markov_adapters.pytorch import CpuTransfer
from markov_control.adapters.runtimes.metadata import MetadataWork


class CpuPointToPoint(CpuTransfer):
    def __init__(self, peer, world) -> None:
        super().__init__(world)
        self.peer = peer

    def install(self) -> None:
        for name in ("send", "recv", "isend", "irecv"):
            operation = getattr(self, name)
            setattr(distributed, name, operation)
            setattr(distributed.distributed_c10d, name, operation)

    def post(self, tensor, group, tag, endpoint, group_endpoint, receiving):
        if tensor.device.type != "cpu":
            raise ValueError("Native metadata transport requires CPU tensors; device transfers belong to graph execution")
        message = self.describe(tensor, group, tag, endpoint, group_endpoint, receiving)
        if not receiving:
            message["values"] = tensor.flatten().tolist()
        handle = self.peer.exchange("metadata_post", message)
        return MetadataWork(self.peer, handle, tensor if receiving else None, "metadata_wait")

    def isend(self, tensor, dst=None, group=None, tag=0, group_dst=None):
        return self.post(tensor, group, tag, dst, group_dst, False)

    def irecv(self, tensor, src=None, group=None, tag=0, group_src=None):
        return self.post(tensor, group, tag, src, group_src, True)

    def send(self, tensor, dst=None, group=None, tag=0, group_dst=None):
        self.isend(tensor, dst, group, tag, group_dst).wait()

    def recv(self, tensor, src=None, group=None, tag=0, group_src=None):
        work = self.irecv(tensor, src, group, tag, group_src)
        work.wait()
        return self.world.index(work.handle[1])


@dataclass
class MetadataTransfer:
    dtype: str
    shape: list
    posted: set = field(default_factory=set)
    values: list | None = None
    waiting: dict = field(default_factory=dict)
    consumed: set = field(default_factory=set)


class PointToPointCalls:
    def __init__(self, observe=False) -> None:
        self.positions = {}
        self.pending = {}
        self.observe = observe
        self.observations = []

    def post(self, channel, rank, message) -> None:
        source, target = message["source"], message["target"]
        receiving = message["receiving"]
        if source == target or rank != (target if receiving else source):
            raise ValueError("Native metadata transfer has an invalid endpoint")
        route = message["group"], source, target, message["tag"]
        endpoint = route, rank
        position = self.positions.get(endpoint, 0)
        key = (*route, position)
        self.positions[endpoint] = position + 1
        transfer = self.pending.setdefault(key, MetadataTransfer(message["dtype"], message["shape"]))
        if (transfer.dtype, transfer.shape) != (message["dtype"], message["shape"]):
            raise ValueError("Native metadata endpoints disagree on tensor dtype or shape")
        transfer.posted.add(rank)
        if not receiving:
            transfer.values = message["values"]
        if self.observe:
            self.observations.append({
                "phase": "post", "key": key, "rank": rank,
                "descriptor": {name: value for name, value in message.items() if name != "values"},
            })

        # Posting is asynchronous. Completion requires the matching receive, even if a sender waits later.
        channel.send(key)
        self.complete(key, transfer)

    def wait(self, channel, rank, handle) -> None:
        key = tuple(handle)
        transfer = self.pending[key]
        if rank not in transfer.posted or rank in transfer.consumed:
            raise ValueError("Native metadata wait has no outstanding local operation")
        transfer.waiting[rank] = channel
        self.complete(key, transfer)

    def complete(self, key, transfer) -> None:
        if len(transfer.posted) != 2:
            return
        for rank, channel in transfer.waiting.items():
            channel.send(transfer.values if rank == key[2] else None)
            transfer.consumed.add(rank)
            if self.observe:
                self.observations.append({"phase": "consumed", "key": key, "rank": rank})
        transfer.waiting.clear()

        # Keep completed payload until both native Work handles have consumed their result.
        if len(transfer.consumed) == 2:
            del self.pending[key]

    def blocked(self):
        return {channel for transfer in self.pending.values() for channel in transfer.waiting.values()}

    def take_observations(self):
        result, self.observations = self.observations, []
        return result
