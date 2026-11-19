"""Expose exported native events without retaining their handles or guessing completion times."""

from contextlib import contextmanager, nullcontext
from contextvars import ContextVar
from dataclasses import dataclass


@dataclass
class ObservedSignal:
    point: int | None
    context: dict
    exported: bool = False


class NativeSignals:
    def __init__(self, current, scope, work_context, runtime=None) -> None:
        self.current = current
        self.scope = scope
        self.work_context = work_context
        self.runtime = runtime
        self.observed = ContextVar("signal_owner", default=None)
        self.started = False

    @property
    def active(self):
        return self.started and self.work_context.calls.enabled

    @contextmanager
    def observe(self):
        self.started = True
        token = self.observed.set(self.current.get())
        try:
            yield
        finally:
            self.observed.reset(token)

    def record(self, original, event, stream=None):
        if not self.active or (self.runtime is not None and self.runtime.recording.get()):
            return original(event, stream)
        if self.current.get() == 0:
            with self.scope(
                "signal_record", original, work=self.work_context.signal_work(stream), device=True, stream=stream,
            ) as call:
                result = self.record_event(original, event, stream)
                self.publish(call, event, "completion")
            return result
        if self.current.get() != self.observed.get():
            return original(event, stream)
        return self.record_event(original, event, stream)

    def record_event(self, original, event, stream):

        # Mark the native record itself; recording a second observer event would change the fence being exported.
        point_scope = nullcontext() if self.runtime is None else self.runtime.native_point(self.current.get(), "signal")
        with point_scope as point:
            result = original(event, stream)

        # The event owns its generation; native Event destruction does not reliably retire weak-key entries.
        event._markov_signal = ObservedSignal(point, self.work_context.signal_work(stream)["context"])
        return result

    def publish(self, call, event, name):
        signal = event._markov_signal
        signal.exported = True
        if signal.point is not None:
            call.signal(name, "output", signal.point)

    def synchronize(self, original, event):
        with self.waiting(original, event, device=False):
            return original(event)

    def query(self, original, event):
        signal = getattr(event, "_markov_signal", None)
        if not self.active or signal is None or not signal.exported:
            return original(event)

        work = self.work_context.work()
        work["context"]["producer"] = signal.context

        # A query bounds completion; its return time is not the device's completion timestamp.
        with self.work_context.calls.read("event", original, work) as observed:
            if signal.point is not None:
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
        if not self.active or signal is None or not signal.exported:
            yield
            return

        if self.current.get() != 0:
            raise ValueError("an exported event wait must have its own action boundary")
        with self.scope(
            "signal_wait", original, work=self.work_context.wait_work(signal.context, device=device, stream=stream),
            bindings={"device_wait": device}, device=device, stream=stream,
        ) as call:
            if signal.point is not None:
                call.signal("completion", "input", signal.point)
            yield
