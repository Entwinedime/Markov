"""Keep native peer output handling while returning its payload to the driver."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import replace
from typing import Any


class SGLangOutputParallel:
    def __init__(self, tp_size: int, rank: int, markers: Any) -> None:
        self.tp_size = tp_size
        self.rank = rank
        self.markers = markers
        self._driver_senders: dict[Any, Any] = {}

    def initialize(self, original: Callable[..., Any], scheduler: Any, *args: Any, **kwargs: Any) -> Any:
        result = original(scheduler, *args, **kwargs)
        # Physical PG1 initialization has finished. Serving branches now see the
        # requested logical role, without rebuilding workers or their KV state.
        scheduler.ps = replace(
            scheduler.ps,
            tp_size=self.tp_size,
            tp_rank=self.rank,
            attn_tp_size=self.tp_size,
            attn_tp_rank=self.rank,
        )
        scheduler.output_streamer.ps = scheduler.ps
        if self.rank == 0:
            return result

        if scheduler.load_snapshot_writer is not None:
            scheduler.load_snapshot_writer.close()
            scheduler.load_snapshot_writer = None

        from sglang.srt.managers.scheduler_components.output_sender import SenderWrapper

        for sender in (scheduler.ipc_channels.send_to_tokenizer, scheduler.ipc_channels.send_to_detokenizer):
            self._driver_senders[sender] = SenderWrapper(sender.socket)
            # Preserve existing references held by the output and flush helpers.
            # They must execute the native peer's socket=None return branch.
            sender.socket = None
        return result

    def initialize_metrics(self, original: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
        # Reuse native role qualification when the context is first constructed,
        # so the representative peer does not inherit root-only CPU statistics.
        kwargs["ps"] = replace(
            kwargs["ps"],
            tp_size=self.tp_size,
            tp_rank=self.rank,
            attn_tp_size=self.tp_size,
            attn_tp_rank=self.rank,
        )
        kwargs["tp_rank"] = self.rank
        return original(*args, **kwargs)

    def send(self, original: Callable[..., Any], sender: Any, output: Any, recv_obj: Any = None) -> Any:
        result = original(sender, output, recv_obj)
        driver = self._driver_senders.get(sender)
        if driver is not None:
            with self.markers.observer():
                # Forward the already constructed payload once. The saved native
                # callable bypasses business wrappers and creates no second Req,
                # output accumulator or completion state machine.
                self._send_output(driver, output, recv_obj)
        return result

    def install(self) -> None:
        from sglang.srt.managers.scheduler_components.output_sender import SenderWrapper
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        self._send_output = SenderWrapper.send_output
        hooks = {
            "sglang.srt.managers.scheduler.Scheduler.__init__": self.initialize,
            "sglang.srt.observability.metrics_collector.SchedulerMetricsCollector.init_new": self.initialize_metrics,
            "sglang.srt.managers.scheduler_components.output_sender.SenderWrapper.send_output": self.send,
        }
        for target, hook in hooks.items():
            HookRegistry.register(target, hook, HookType.AROUND)
