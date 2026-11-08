"""Expose exported native events without retaining their handles or guessing completion times."""

from contextlib import contextmanager
from contextvars import ContextVar
from dataclasses import dataclass


@dataclass
class ObservedSignal:
    point: int
    context: dict
    exported: bool = False


class NativeSignals:
    def __init__(self, capture, work_context) -> None:
        self.capture = capture
        self.work_context = work_context
        self.observed = ContextVar("signal_owner", default=None)
        self.enabled = False

    @contextmanager
    def observe(self, call):
        self.enabled = True
        token = self.observed.set(call)
        try:
            yield
        finally:
            self.observed.reset(token)

    def record(self, original, event, stream=None):
        if self.capture.runtime.recording.get():
            return original(event, stream)
        call = self.observed.get()
        if self.enabled and self.capture.current.get() == 0:
            with self.capture.call(
                "signal_record", original.__qualname__, f"{original.__module__}.{original.__qualname__}",
                parameters={}, work=self.work_context.signal_work(stream), device=True, stream=stream,
            ) as call:
                result = self.record_event(call, original, event, stream)
                self.publish(call, event, "completion")
            self.capture.runtime.retire()
            return result
        if call is None or self.capture.current.get() != call.identity:
            return original(event, stream)
        return self.record_event(call, original, event, stream)

    def record_event(self, call, original, event, stream):

        # Mark the native record itself; recording a second observer event would change the fence being exported.
        with self.capture.runtime.native_point(call.identity, "signal") as point:
            result = original(event, stream)

        # The event owns its generation; native Event destruction does not reliably retire weak-key entries.
        event._markov_signal = ObservedSignal(point, self.work_context.signal_work(stream)["context"])
        return result

    def publish(self, call, event, name):
        signal = event._markov_signal
        signal.exported = True
        call.signal(name, "output", signal.point)

    def synchronize(self, original, event):
        with self.waiting(original, event, device=False):
            return original(event)

    def query(self, original, event):
        signal = getattr(event, "_markov_signal", None)
        if not self.enabled or signal is None or not signal.exported:
            return original(event)

        work = self.work_context.work()
        work["context"]["producer"] = signal.context

        # A query bounds completion; its return time is not the device's completion timestamp.
        with self.work_context.calls.read("event", original, work) as observed:
            observed["point"] = signal.point
            result = original(event)
            observed["result"] = result
        return result

    def wait(self, original, event, stream=None):
        with self.waiting(original, event, device=True, stream=stream):
            return original(event, stream)

    @contextmanager
    def waiting(self, original, event, *, device, stream=None):
        signal = getattr(event, "_markov_signal", None)
        if not self.enabled or signal is None or not signal.exported:
            yield
            return

        if self.capture.current.get() != 0:
            raise ValueError("an exported event wait must have its own action boundary")
        with self.capture.call(
            "signal_wait", original.__qualname__, f"{original.__module__}.{original.__qualname__}",
            parameters={}, work=self.work_context.wait_work(signal.context, device=device, stream=stream),
            bindings={"device_wait": device}, device=device, stream=stream,
        ) as call:
            call.signal("completion", "input", signal.point)
            yield
