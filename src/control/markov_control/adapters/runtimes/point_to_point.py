"""Expose native CPU metadata sends and receives without hiding blocked workers inside Gloo."""

from dataclasses import dataclass, field

import torch.distributed as distributed

from markov_adapters.pytorch import CpuTransfer
from markov_control.adapters.runtimes.metadata import MetadataWork


class CpuPointToPoint(CpuTransfer):
    def __init__(self, peer, world, context) -> None:
        super().__init__(world)
        self.peer, self.context = peer, context

    def install(self) -> None:
        for name in ("send", "recv", "isend", "irecv"):
            operation = getattr(self, name)
            setattr(distributed, name, operation)
            setattr(distributed.distributed_c10d, name, operation)

    def post(self, tensor, group, tag, endpoint, group_endpoint, receiving):
        if tensor.device.type != "cpu":
            raise ValueError("Native metadata transport requires CPU tensors; device transfers belong to graph execution")
        execution = self.context.get()
        if execution is None:
            raise ValueError("CPU metadata post requires its native protocol context")
        message = self.describe(tensor, group, tag, endpoint, group_endpoint, receiving)
        if not receiving:
            message["values"] = tensor.flatten().tolist()
        handle = self.peer.exchange("metadata_post", {**message, "execution": execution})
        return MetadataWork(self.peer, handle, tensor if receiving else None, "metadata_wait", context=self.context)

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
    values: list | None = None
    consumed: set = field(default_factory=set)


class PointToPointCalls:
    """Keep payloads and native channel positions; the core owns readiness and suspended consumers."""

    def __init__(self) -> None:
        self.positions = {}
        self.pending = {}

    def post(self, rank, message):
        source, target = message["source"], message["target"]
        receiving = message["receiving"]
        if source == target or rank != (target if receiving else source):
            raise ValueError("native metadata transfer has an invalid endpoint")
        route = message["group"], source, target, message["tag"]
        endpoint = route, rank
        position = self.positions.get(endpoint, 0)
        key = (*route, position)
        self.positions[endpoint] = position + 1
        transfer = self.pending.setdefault(key, MetadataTransfer())
        if not receiving:
            transfer.values = message["values"]
        return key

    def consume(self, rank, handle):
        key = tuple(handle)
        transfer = self.pending[key]
        if rank not in key[1:3] or rank in transfer.consumed or transfer.values is None:
            raise ValueError("core released a metadata consumer without a live matching payload")
        transfer.consumed.add(rank)
        values = transfer.values if rank == key[2] else None
        if len(transfer.consumed) == 2:
            del self.pending[key]
        return values
