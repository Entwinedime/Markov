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

    def observer(self) -> AbstractContextManager:
        """Identify collection work separately from the observed native body."""
        return self.scope("markov_observer")


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

    def scope(
        self, step: str, metadata: dict[str, Any], *, binding: str | None = None, pending: bool = False
    ) -> Observation:
        self._sequence += 1
        return Observation(self, self._sequence, step, metadata, binding, pending=pending)

    def set_scope(self, identity: int | None) -> None:
        self._local.current = identity
        if self._native is not None:
            self._native.markov_native_set_scope(0 if identity is None else identity)

    def in_scope(self) -> bool:
        return getattr(self._local, "current", None) is not None

    def write(self, record: dict[str, Any]) -> None:
        with self.markers.observer():
            record["rank"] = self.rank
            record["pid"] = os.getpid()
            record["tid"] = threading.get_native_id()
            pending = getattr(self._local, "pending", None)
            if pending is not None:
                pending.records.append(record)
                return
            self._write(record)
            self._flush()

    def _write(self, record: dict[str, Any]) -> None:
        if self._stream is None:
            self.directory.mkdir(parents=True, exist_ok=True)
            self._stream = (self.directory / f"rank-{self.rank}-pid-{os.getpid()}.jsonl").open("a", encoding="utf-8")

        self._stream.write(json.dumps(record, separators=(",", ":")) + "\n")

    def _flush(self) -> None:
        # Nested records stay buffered until their enclosing scope closes.
        if getattr(self._local, "current", None) is None:
            self._stream.flush()
            self.flush_native(close_window=False)

    def finish_pending(self, observation: Observation) -> None:
        with self.markers.observer():
            self._local.pending = None
            if observation.committed:
                for record in observation.records:
                    self._write(record)
                # Publish the complete committed group before acknowledging it.
                self._flush()
            observation.records.clear()

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
        self,
        recorder: Recorder,
        identity: int,
        step: str,
        metadata: dict[str, Any],
        binding: str | None,
        *,
        pending: bool = False,
    ) -> None:
        self.recorder = recorder
        self.identity = identity
        self.step = step
        self.metadata = metadata
        self.binding = binding
        self.result: dict[str, Any] = {}
        self.pending = pending
        self.committed = not pending
        self.records: list[dict[str, Any]] = []

    def commit(self) -> None:
        self.committed = True

    def __enter__(self) -> Observation:
        self.parent = getattr(self.recorder._local, "current", None)
        if self.pending:
            if getattr(self.recorder._local, "pending", None) is not None:
                raise RuntimeError("a pending observation is already active")
            self.recorder._local.pending = self
        self.recorder.set_scope(self.identity)

        # Marker lifecycle and scalar clock reads stay outside observer ranges.
        # Wrapping marker entry/exit would break the native profiler stack.
        try:
            prefix = "markov_pending" if getattr(self.recorder._local, "pending", None) is not None else "markov"
            self._marker = self.recorder.markers.scope(f"{prefix}/{self.step}/{self.identity}")
            self._marker.__enter__()
        except BaseException:
            self.recorder.set_scope(self.parent)
            if self.pending:
                self.recorder._local.pending = None
            raise

        self.start_ns = time.perf_counter_ns()

        return self

    def __exit__(self, exc_type: Any, exc_value: Any, traceback: Any) -> None:
        end_ns = time.perf_counter_ns()

        # Restore the enclosing scope before persisting this observation.
        try:
            self._marker.__exit__(exc_type, exc_value, traceback)
        finally:
            self.recorder.set_scope(self.parent)

        self.recorder.write(
            {
                "call_id": self.identity,
                "parent_id": self.parent,
                "step": self.step,
                "binding": self.binding,
                "start_ns": self.start_ns,
                "end_ns": end_ns,
                "metadata": self.metadata,
                "result": self.result,
                "status": "completed" if exc_type is None else "failed",
            }
        )
        if self.pending:
            self.recorder.finish_pending(self)
