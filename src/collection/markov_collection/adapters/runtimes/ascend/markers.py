"""Record real stream boundaries without synchronizing the serving path."""

from __future__ import annotations

import ctypes
import os
import threading
from contextlib import contextmanager
from contextvars import ContextVar
from typing import Any

import torch_npu

from markov_collection.adapters.runtimes.ascend.clock import AscendClock


class AscendMarkers:
    def __init__(self) -> None:
        self.points: list[dict[str, Any]] = []
        self.events: list[Any] = []
        self.lock = threading.Lock()
        self.recording = ContextVar("observer_point", default=False)
        self.clock_samples = []
        self.intervals = []
        self.host_sample = ctypes.CDLL(None).markov_sample_host
        self.host_sample.argtypes = [ctypes.POINTER(ctypes.c_uint64)]
        self.host_sample.restype = None
        self.native_service = ctypes.CDLL(None).markov_set_service_scope
        self.native_service.argtypes = [ctypes.c_int64]
        self.native_service.restype = ctypes.c_int64

    def calibrate(self) -> None:
        self.clock_samples.append(AscendClock.sample(torch_npu.npu.current_device()))

    def timestamp(self, record, side):
        sample = (ctypes.c_uint64 * 2)()
        self.host_sample(sample)
        record[f"{side}_counter"], record[f"{side}_raw_ns"] = sample

    def host_observation(self):
        return {"process": os.getpid(), "intervals": self.intervals}

    @contextmanager
    def scope(self, name: str):
        if len(self.clock_samples) != 1:
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

        # Keep in-flight events alive; the API log records handle lifetimes when completed markers are released.
        event = torch_npu.npu.Event()
        with self.native_point(scope, side) as identity:
            event.record(stream)
        self.events.append(event)
        return identity

    def retire(self) -> None:
        with self.scope("markov.observer"):
            self.events = [event for event in self.events if not event.query()]

    def release(self) -> None:
        with self.scope("markov.observer"):
            self.events.clear()
