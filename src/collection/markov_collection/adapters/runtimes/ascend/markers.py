"""Record real stream boundaries without synchronizing the serving path."""

from __future__ import annotations

import ctypes
import os
import threading
import time
from contextlib import contextmanager
from contextvars import ContextVar
from typing import Any

import torch_npu

from markov_collection.adapters.runtimes.ascend.clock import AscendClock


class AscendMarkers:
    def __init__(self) -> None:
        self.points: list[dict[str, Any]] = []
        self.stream_events: dict[Any, Any] = {}
        self.lock = threading.Lock()
        self.recording = ContextVar("observer_point", default=False)
        self.clock_samples = []
        self.observing = False
        self.intervals = []

        # These bounded CPU-only helpers must not release the GIL just to read a clock or exchange a thread-local value.
        bridge = ctypes.PyDLL(None)
        self.host_sample = bridge.markov_sample_host
        self.host_sample.argtypes = [ctypes.POINTER(ctypes.c_uint64)]
        self.host_sample.restype = None
        self.native_service = bridge.markov_set_service_scope
        self.native_service.argtypes = [ctypes.c_int64]
        self.native_service.restype = ctypes.c_int64

    @contextmanager
    def window(self):
        self.clock_samples.append(AscendClock.sample(torch_npu.npu.current_device()))
        self.observing = True
        try:
            with self.scope("markov.window"):
                yield
        finally:
            self.observing = False
            self.clock_samples.append(AscendClock.sample(torch_npu.npu.current_device()))

    def checkpoint(self) -> None:
        if not self.observing:
            return
        elapsed_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW) - self.clock_samples[-1]["end_raw_ns"]
        if elapsed_ns < 1_000_000_000:
            return

        # Sampling cadence controls observation overhead; the actual independent snapshots define the clock segments.
        with self.scope("markov.observer"):
            self.clock_samples.append(AscendClock.sample(torch_npu.npu.current_device()))

    def timestamp(self, record, side):
        sample = (ctypes.c_uint64 * 2)()
        self.host_sample(sample)
        record[f"{side}_counter"], record[f"{side}_raw_ns"] = sample

    def host_observation(self):
        return {"process": os.getpid(), "intervals": self.intervals}

    @contextmanager
    def scope(self, name: str):
        if not self.observing:
            yield
            return
        record = {"name": name, "thread": threading.get_native_id()}
        self.intervals.append(record)
        with self.service_scope(record):
            yield

    @contextmanager
    def service_scope(self, interval):

        # Ordering uses the same CPU counter as SDK enqueue/API records; measurements stay in RAW time.
        self.timestamp(interval, "begin")
        try:
            yield
        finally:
            self.timestamp(interval, "end")

    @contextmanager
    def service_execution(self, scope: int, execution: dict):

        # Driver submissions carry the caller identity even on native executor threads without Torch callbacks.
        previous = self.native_service(scope)
        execution["thread"] = threading.get_native_id()
        try:
            with self.service_scope(execution):
                yield
        finally:
            self.native_service(previous)

    @contextmanager
    def native_point(self, scope: int, side: str):
        with self.lock:
            identity = len(self.points)
            self.points.append({"scope": scope, "side": side})
        token = self.recording.set(True)
        try:
            with self.scope(f"markov.point/{identity}"):
                yield identity
        finally:
            self.recording.reset(token)

    def point(self, scope: int, side: str, stream=None) -> int:
        stream = torch_npu.npu.current_stream() if stream is None else stream

        # Nobody waits on observer handles; each repeated record has its own SDK measurement and point identity.
        with self.native_point(scope, side) as identity, self.lock:
            event = self.stream_events.get(stream)
            if event is None:
                event = torch_npu.npu.Event()
                self.stream_events[stream] = event
            event.record(stream)
        return identity

    def release(self) -> None:
        with self.scope("markov.observer"), self.lock:
            self.stream_events.clear()
