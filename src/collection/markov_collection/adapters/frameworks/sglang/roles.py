"""Run native root/peer control paths with auxiliary transport to the local driver."""

from __future__ import annotations

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


class WorkerRole:
    def __init__(self, size: int, rank: int, values: Any) -> None:
        from sglang.srt.managers.scheduler_components.output_sender import SenderWrapper
        from sglang.srt.managers.scheduler_components.request_receiver import SchedulerRequestReceiver
        from sglang.srt.utils import broadcast_pyobj
        from torch.distributed import broadcast

        self.size = size
        self.rank = rank
        self.values = values

        self.payload: BroadcastPayload | None = None
        self.driver_senders: dict[Any, Any] = {}

        self.native_send = SenderWrapper.send_output
        self.native_pull = SchedulerRequestReceiver._pull_raw_reqs
        self.native_pyobj = broadcast_pyobj
        self.native_broadcast = broadcast

    def topology(self, state: Any) -> Any:
        return replace(state, tp_size=self.size, tp_rank=self.rank, attn_tp_size=self.size, attn_tp_rank=self.rank)

    def receiver(self, original: Any, scheduler: Any) -> Any:
        result = original(scheduler)
        receiver = scheduler.request_receiver
        group = ReceiverGroup(self.rank, tuple(range(self.size)))
        scheduler.request_receiver = replace(
            receiver, ps=self.topology(receiver.ps), tp_group=group, attn_tp_group=group
        )
        return result

    def initialized(self, original: Any, scheduler: Any, *args: Any, **kwargs: Any) -> Any:
        from sglang.srt.managers.scheduler_components.output_sender import SenderWrapper

        result = original(scheduler, *args, **kwargs)
        scheduler.ps = self.topology(scheduler.ps)
        scheduler.output_streamer.ps = scheduler.ps
        if self.rank == 0:
            return result

        # Peers follow native no-output behavior; auxiliary senders still return results to the local driver.
        if scheduler.load_snapshot_writer is not None:
            scheduler.load_snapshot_writer.close()
            scheduler.load_snapshot_writer = None
        for sender in (scheduler.ipc_channels.send_to_tokenizer, scheduler.ipc_channels.send_to_detokenizer):
            self.driver_senders[sender] = SenderWrapper(sender.socket)
            sender.socket = None

        return result

    def metrics(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        kwargs["ps"] = self.topology(kwargs["ps"])
        kwargs["tp_rank"] = self.rank
        return original(*args, **kwargs)

    def receive(self, original: Any, receiver: Any, requests: Any) -> Any:
        if self.rank == 0:
            return original(receiver, requests)

        # Supply one real serialized message; native peer allocation and deserialization remain intact.
        with self.values.auxiliary():
            source = replace(receiver, ps=replace(receiver.ps, tp_rank=0, attn_tp_rank=0))
            incoming = self.native_pull(source)
            self.payload = BroadcastPayload(receiver.tp_cpu_group)

        try:
            with self.values.auxiliary():
                self.native_pyobj(incoming, 0, receiver.tp_cpu_group)
                self.payload.position = 0

            return original(receiver, requests)
        finally:
            self.payload = None

    def broadcast(self, original: Any, tensor: Any, *args: Any, **kwargs: Any) -> Any:
        if self.payload is not None and kwargs.get("group") is self.payload.group:
            with self.values.auxiliary():
                source = self.payload.position is None
                self.payload.exchange(tensor)
            if source:
                return self.native_broadcast(tensor, *args, **kwargs)

        return original(tensor, *args, **kwargs)

    def send(self, original: Any, sender: Any, output: Any, recv_obj: Any = None) -> Any:
        result = original(sender, output, recv_obj)
        driver = self.driver_senders.get(sender)
        if driver is not None:
            with self.values.auxiliary():
                self.native_send(driver, output, recv_obj)

        return result

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        targets = {
            "sglang.srt.managers.scheduler.Scheduler.__init__": self.initialized,
            "sglang.srt.managers.scheduler.Scheduler.init_request_receiver": self.receiver,
            (
                "sglang.srt.managers.scheduler_components.request_receiver."
                "SchedulerRequestReceiver._broadcast_reqs_across_ranks"
            ): self.receive,
            "sglang.srt.managers.scheduler_components.output_sender.SenderWrapper.send_output": self.send,
            "sglang.srt.observability.metrics_collector.SchedulerMetricsCollector.init_new": self.metrics,
            "torch.distributed.broadcast": self.broadcast,
        }
        for target, hook in targets.items():
            HookRegistry.register(target, hook, HookType.AROUND)
