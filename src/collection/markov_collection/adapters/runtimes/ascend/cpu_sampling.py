"""Read calling-thread service counters from the installed Ascend observation library."""

from __future__ import annotations

import ctypes
from pathlib import Path
from types import TracebackType


class CpuCounters(ctypes.Structure):
    _fields_ = [
        (name, ctypes.c_int64)
        for name in (
            "wait_ns",
            "probe_ns",
            "wait_wall_ns",
            "last_sync_start_ns",
            "last_sync_end_ns",
            "last_sync_cpu_end_ns",
            "last_sync_probe_cpu_ns",
            "last_sync_result",
        )
    ]


class CpuSampling:
    """Own the native ABI binding; reading does not observe other worker threads."""

    def __init__(self, worker_directory: Path) -> None:
        self._native = ctypes.CDLL(None)
        self._native.markov_native_begin_cpu_sampling.argtypes = []
        self._native.markov_native_begin_cpu_sampling.restype = None
        self._native.markov_native_read_cpu_counters.argtypes = [ctypes.POINTER(CpuCounters)]
        self._native.markov_native_read_cpu_counters.restype = None

        self._native.markov_native_begin_worker_sampling.argtypes = [ctypes.c_char_p]
        self._native.markov_native_begin_worker_sampling.restype = None
        self._native.markov_native_worker_scope.argtypes = [ctypes.c_int]
        self._native.markov_native_worker_scope.restype = ctypes.c_int
        self._native.markov_native_flush_worker_sampling.argtypes = []
        self._native.markov_native_flush_worker_sampling.restype = ctypes.c_int
        self._scopes: list[int] = []
        self._native.markov_native_begin_worker_sampling(str(worker_directory).encode())

        # Sampling mode counts waits without enabling the profiler or API journal.
        self._native.markov_native_begin_cpu_sampling()

    def read(self) -> CpuCounters:
        # Each call copies the current thread's cumulative counters. The caller
        # chooses its semantic boundaries and computes deltas on that thread.
        counters = CpuCounters()
        self._native.markov_native_read_cpu_counters(ctypes.byref(counters))
        return counters

    def __enter__(self) -> CpuSampling:
        self._scopes.append(self._native.markov_native_worker_scope(1))
        return self

    def __exit__(
        self,
        exception_type: type[BaseException] | None,
        exception: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self._native.markov_native_worker_scope(self._scopes.pop())

    def flush_workers(self) -> None:
        if self._native.markov_native_flush_worker_sampling() != 0:
            raise RuntimeError("native worker sampling flush failed")
