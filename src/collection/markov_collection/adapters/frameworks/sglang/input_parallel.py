"""Run native receiver roles over the representative worker's real CPU group."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass, replace
from typing import Any


@dataclass(frozen=True, slots=True)
class ReceiverGroup:
    rank: int
    ranks: tuple[int, ...]


class BroadcastPayload:
    def __init__(self, group: Any) -> None:
        self.group = group
        self.tensors: list[Any] = []
        self.position: int | None = None

    def exchange(self, tensor: Any) -> None:
        if self.position is None:
            self.tensors.append(tensor)
        else:
            tensor.copy_(self.tensors[self.position])
            self.position += 1


class SGLangInputParallel:
    def __init__(self, tp_size: int, rank: int, markers: Any) -> None:
        self.tp_size = tp_size
        self.rank = rank
        self.markers = markers
        self._payload: BroadcastPayload | None = None

    def initialize(self, original: Callable[..., Any], scheduler: Any) -> Any:
        result = original(scheduler)
        receiver = scheduler.request_receiver
        group = ReceiverGroup(self.rank, tuple(range(self.tp_size)))
        scheduler.request_receiver = replace(
            receiver,
            ps=replace(
                receiver.ps,
                tp_size=self.tp_size,
                tp_rank=self.rank,
                attn_tp_size=self.tp_size,
                attn_tp_rank=self.rank,
            ),
            tp_group=group,
            attn_tp_group=group,
        )
        return result

    def broadcast_requests(self, original: Callable[..., Any], receiver: Any, recv_reqs: Any) -> Any:
        if self.rank == 0:
            return original(receiver, recv_reqs)

        # Obtain an actual source payload for this call only. Calling the saved
        # native functions keeps auxiliary receive/serialization out of semantic
        # wrappers; the peer below still executes native allocation and loads.
        with self.markers.observer():
            source = replace(receiver, ps=replace(receiver.ps, tp_rank=0, attn_tp_rank=0))
            source_requests = self._pull_raw_requests(source)
            payload = BroadcastPayload(receiver.tp_cpu_group)
            self._payload = payload
        try:
            with self.markers.observer():
                self._broadcast_pyobj(source_requests, 0, receiver.tp_cpu_group)
                payload.position = 0
            return original(receiver, recv_reqs)
        finally:
            with self.markers.observer():
                self._payload = None

    def broadcast(self, original: Callable[..., Any], tensor: Any, *args: Any, **kwargs: Any) -> Any:
        payload = self._payload
        if payload is not None and kwargs.get("group") is payload.group:
            with self.markers.observer():
                source = payload.position is None
                payload.exchange(tensor)
            if source:
                # broadcast_pyobj resolves dist.broadcast dynamically. Dispatch
                # its auxiliary source straight to native code, before any
                # business wrapper can create a message or advance its ordinal.
                return self._native_broadcast(tensor, *args, **kwargs)
        # CPU PG1 supplies the physical call. Auxiliary tensor filling is not a
        # replacement for the peer's native pickle.loads after this transport.
        return original(tensor, *args, **kwargs)

    def install(self) -> None:
        from sglang.srt.managers.scheduler_components.request_receiver import SchedulerRequestReceiver
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType
        from sglang.srt.utils import broadcast_pyobj
        from torch.distributed import broadcast

        self._pull_raw_requests = SchedulerRequestReceiver._pull_raw_reqs
        self._broadcast_pyobj = broadcast_pyobj
        self._native_broadcast = broadcast
        hooks = {
            "sglang.srt.managers.scheduler.Scheduler.init_request_receiver": self.initialize,
            (
                "sglang.srt.managers.scheduler_components.request_receiver."
                "SchedulerRequestReceiver._broadcast_reqs_across_ranks"
            ): self.broadcast_requests,
        }
        for target, hook in hooks.items():
            HookRegistry.register(target, hook, HookType.AROUND)

    def install_dispatch(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        # Last registered is outermost: helper filling precedes the observed
        # call, while auxiliary source serialization bypasses its whole chain.
        HookRegistry.register("torch.distributed.broadcast", self.broadcast, HookType.AROUND)
