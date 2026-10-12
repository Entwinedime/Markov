"""Scalar observations and profiler identities, without tensor snapshots or synchronization."""

from __future__ import annotations

import atexit
import ctypes
import json
import os
import threading
import time
from abc import ABC, abstractmethod
from contextlib import AbstractContextManager
from pathlib import Path
from typing import Any, TextIO


class ProfilerMarkers(ABC):
    @abstractmethod
    def scope(self, name: str) -> AbstractContextManager:
        """Return a native profiler range without starting or synchronizing it."""


class Recorder:
    def __init__(self, directory: Path, rank: int, markers: ProfilerMarkers) -> None:
        self.markers = markers
        self.directory = directory
        self.rank = rank
        self._stream: TextIO | None = None
        self._sequence = 0
        self._local = threading.local()
        self._native: ctypes.CDLL | None = None

        native_directory = os.environ.get("MARKOV_NATIVE_OBSERVATIONS")
        if native_directory is not None:
            self._native = ctypes.CDLL(None)
            self._native.markov_native_begin_window.argtypes = [ctypes.c_char_p]
            self._native.markov_native_begin_window.restype = None
            self._native.markov_native_flush.argtypes = [ctypes.c_int]
            self._native.markov_native_flush.restype = ctypes.c_int
            self._native.markov_native_set_scope.argtypes = [ctypes.c_uint64]
            self._native.markov_native_set_scope.restype = None
            self._native.markov_native_begin_window(os.fsencode(native_directory))

        atexit.register(self.close)

    def scope(self, step: str, metadata: dict[str, Any], *, binding: str | None = None) -> Observation:
        self._sequence += 1
        return Observation(self, self._sequence, step, metadata, binding)

    def set_scope(self, identity: int | None) -> None:
        self._local.current = identity
        if self._native is not None:
            self._native.markov_native_set_scope(0 if identity is None else identity)

    def in_scope(self) -> bool:
        return getattr(self._local, "current", None) is not None

    def write(self, record: dict[str, Any]) -> None:
        if self._stream is None:
            self.directory.mkdir(parents=True, exist_ok=True)
            self._stream = (self.directory / f"rank-{self.rank}-pid-{os.getpid()}.jsonl").open("a", encoding="utf-8")

        record["rank"] = self.rank
        record["pid"] = os.getpid()
        record["tid"] = threading.get_native_id()

        self._stream.write(json.dumps(record, separators=(",", ":")) + "\n")

        # Explicit flushes belong to the outer serving action. Nested records
        # still use normal file buffering; adding region markers must not force
        # a Python/native flush for every module or layer.
        if getattr(self._local, "current", None) is None:
            self._stream.flush()
            self.flush_native(close_window=False)

    def flush_native(self, *, close_window: bool) -> None:
        if self._native is not None and self._native.markov_native_flush(int(close_window)) != 0:
            raise RuntimeError("native observation could not be persisted; see the worker log")

    def close(self) -> None:
        self.flush_native(close_window=True)
        if self._stream is not None:
            self._stream.close()
            self._stream = None


class Observation:
    def __init__(
        self, recorder: Recorder, identity: int, step: str, metadata: dict[str, Any], binding: str | None
    ) -> None:
        self.recorder = recorder
        self.identity = identity
        self.step = step
        self.metadata = metadata
        self.binding = binding
        self.result: dict[str, Any] = {}

    def __enter__(self) -> Observation:
        self.parent = getattr(self.recorder._local, "current", None)
        self.recorder.set_scope(self.identity)

        self._marker = self.recorder.markers.scope(f"markov/{self.step}/{self.identity}")
        self._marker.__enter__()

        self.thread_start_ns = time.thread_time_ns()
        self.wall_time_ns = time.time_ns()
        self.start_ns = time.perf_counter_ns()

        return self

    def __exit__(self, exc_type: Any, exc_value: Any, traceback: Any) -> None:
        # Keep thread-clock sampling outside the interval of the observed call.
        end_ns = time.perf_counter_ns()
        thread_end_ns = time.thread_time_ns()

        # Restore the enclosing scope before persisting this observation.
        self._marker.__exit__(exc_type, exc_value, traceback)
        self.recorder.set_scope(self.parent)

        self.recorder.write(
            {
                "call_id": self.identity,
                "parent_id": self.parent,
                "step": self.step,
                "binding": self.binding,
                "start_ns": self.start_ns,
                "end_ns": end_ns,
                "thread_cpu_ns": thread_end_ns - self.thread_start_ns,
                "wall_time_ns": self.wall_time_ns,
                "metadata": self.metadata,
                "result": self.result,
                "status": "completed" if exc_type is None else "failed",
            }
        )
