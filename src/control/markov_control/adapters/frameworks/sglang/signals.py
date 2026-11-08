"""Bind native event handles to action completion ports; execution remains in the core timeline."""

from contextlib import contextmanager
from dataclasses import dataclass
from weakref import WeakValueDictionary

from markov_adapters.sglang.pipeline import PipelineABI


@dataclass
class ControlSignal:
    identity: str
    context: dict


class PipelineSignals:
    def __init__(self, scheduler) -> None:
        self.scheduler = scheduler
        self.live = WeakValueDictionary()
        self.sequence = 0
        self.enclosing = 0

    @contextmanager
    def producing(self):
        self.enclosing += 1
        try:
            yield
        finally:
            self.enclosing -= 1

    def record(self, original, event, stream=None):
        result = original(event, stream)
        work = self.scheduler.pipeline_messages.signal_work(stream)
        identity = self.publish(event, work["context"])
        if not self.enclosing:
            work["signals"] = {"completion": identity}
            self.scheduler.action("signal_record", {}, work)
        return result

    def publish(self, event, context):

        # Re-recording a handle creates a fresh generation; old waits retain their already submitted graph edge.
        identity = f"{self.scheduler.group}/signal/{self.sequence}"
        self.sequence += 1
        signal = ControlSignal(identity, context)
        event._markov_signal = signal

        # Observe Python signal lifetimes, so the core retains completions only while native events own them.
        self.live[identity] = signal
        return identity

    def wait(self, event, *, device, stream=None):
        signal = event._markov_signal
        work = self.scheduler.pipeline_messages.wait_work(signal.context, device=device, stream=stream)
        work["signals"] = {"completion": signal.identity}
        self.scheduler.action("signal_wait", {}, work, condition={"device_wait": device})

    def wait_stream(self, original, stream, other):
        result = original(stream, other)
        if not self.enclosing:
            streams = PipelineABI.streams(self.scheduler, stream, other)
            work = self.scheduler.pipeline_messages.work()
            work["context"]["streams"] = streams
            self.scheduler.action("stream_wait", {}, work, condition={"streams": streams})
        return result

    def query(self, original, event):
        signal = getattr(event, "_markov_signal", None)
        if signal is None:
            return original(event)
        work = self.scheduler.pipeline_messages.work()
        work["context"]["producer"] = signal.context
        with self.scheduler.calls.read("event", original, work) as query:
            result = self.scheduler.peer.call("completed", signal=signal.identity, observation=query["observation"])
            query["result"] = result
            return result

    def synchronize(self, original, event):
        self.wait(event, device=False)

    def wait_event(self, original, stream, event):
        self.wait(event, device=True, stream=stream)

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        HookRegistry.register("torch.cpu.Event.query", self.query, HookType.AROUND)
        HookRegistry.register("torch.cpu.Event.synchronize", self.synchronize, HookType.AROUND)
        HookRegistry.register("torch.cpu.Event.record", self.record, HookType.AROUND)
        HookRegistry.register("torch.cpu.Stream.wait_event", self.wait_event, HookType.AROUND)
        HookRegistry.register("torch.cpu.Stream.wait_stream", self.wait_stream, HookType.AROUND)
