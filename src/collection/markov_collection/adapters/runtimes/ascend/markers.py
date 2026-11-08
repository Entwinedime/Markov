"""Record real stream boundaries without synchronizing the serving path."""

from __future__ import annotations

import ctypes
import threading
import time
from contextlib import contextmanager
from contextvars import ContextVar
from typing import Any

import torch
import torch_npu

from markov_collection.adapters.runtimes.ascend.clock import AscendClock


class AscendMarkers:
    def __init__(self) -> None:
        self.points: list[dict[str, Any]] = []
        self.events: list[Any] = []
        self.lock = threading.Lock()
        self.recording = ContextVar("observer_point", default=False)
        self.clock_samples = []
        self.native_service = ctypes.CDLL(None).markov_set_service_scope
        self.native_service.argtypes = [ctypes.c_int64]
        self.native_service.restype = ctypes.c_int64

    def calibrate(self) -> None:
        self.clock_samples.append(AscendClock.sample(torch_npu.npu.current_device()))

    @staticmethod
    def scope(name: str) -> Any:
        return torch.profiler.record_function(name)

    @staticmethod
    @contextmanager
    def service_scope(interval):

        # Native transport threads do not inherit Torch's thread-local profiler callbacks.
        interval["begin_raw_ns"] = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
        try:
            yield
        finally:
            interval["end_raw_ns"] = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)

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
