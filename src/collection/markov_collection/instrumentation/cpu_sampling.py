"""Independent host clocks, with observer work excluded from thread CPU time."""

from __future__ import annotations

import ctypes
import os
from abc import ABC, abstractmethod
from collections.abc import Callable, Iterator
from contextlib import AbstractContextManager, contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Any

CpuBoundaryKey = tuple[str, str, str, str, int, int]


@dataclass(frozen=True, slots=True)
class CpuAnchor:
    key: CpuBoundaryKey
    scope_id: int
    wall_ns: int
    thread_cpu_ns: int
    causal_cut: bool = False

    @staticmethod
    def boundary_key(value: dict[str, Any]) -> CpuBoundaryKey:
        return value["role"], value["kind"], value["edge"], value["name"], value["ordinal"], value["count"]


class CpuBoundaryReader(ABC):
    @abstractmethod
    def read(self, lanes: dict[tuple[int, int], list[dict[str, Any]]]) -> dict[tuple[int, int], list[CpuAnchor]]:
        pass


class CpuObserver:
    """Native exclusion shared by CPU clocks and queued sampling work."""

    def __init__(self) -> None:
        self._native = ctypes.CDLL(None)
        self._native.markov_cpu_observer_begin.argtypes = []
        self._native.markov_cpu_observer_begin.restype = None
        self._native.markov_cpu_observer_end.argtypes = []
        self._native.markov_cpu_observer_end.restype = None

    @contextmanager
    def observer(self) -> Iterator[None]:
        self._native.markov_cpu_observer_begin()
        try:
            yield
        finally:
            self._native.markov_cpu_observer_end()


class CpuSampling(CpuObserver):
    def __init__(self, directory: Path) -> None:
        super().__init__()
        self._native.markov_cpu_begin_sampling.argtypes = [ctypes.c_char_p]
        self._native.markov_cpu_begin_sampling.restype = None
        self._native.markov_cpu_scope_begin.argtypes = [ctypes.c_uint64, ctypes.c_char_p]
        self._native.markov_cpu_scope_begin.restype = ctypes.c_void_p
        self._native.markov_cpu_scope_end.argtypes = [ctypes.c_void_p]
        self._native.markov_cpu_scope_end.restype = None
        self._native.markov_cpu_function_begin.argtypes = [ctypes.c_char_p, ctypes.c_uint64]
        self._native.markov_cpu_function_begin.restype = ctypes.c_void_p
        self._native.markov_cpu_function_end.argtypes = [ctypes.c_void_p]
        self._native.markov_cpu_function_end.restype = None
        self._native.markov_cpu_flush.argtypes = []
        self._native.markov_cpu_flush.restype = ctypes.c_int
        self._native.markov_cpu_begin_sampling(os.fsencode(directory))

    @contextmanager
    def scope(self, identity: int, name: str) -> Iterator[None]:
        handle = self._native.markov_cpu_scope_begin(identity, name.encode())
        try:
            yield
        finally:
            self._native.markov_cpu_scope_end(handle)

    @contextmanager
    def function(self, name: bytes, context_id: int = 0) -> Iterator[None]:
        # Nonzero identities index work metadata on the enclosing serving scope.
        handle = self._native.markov_cpu_function_begin(name, context_id)
        try:
            yield
        finally:
            self._native.markov_cpu_function_end(handle)

    def flush(self) -> None:
        if self._native.markov_cpu_flush() != 0:
            raise RuntimeError("native CPU sampling flush failed")


class CpuFunction:
    """One named native range; input arguments and model tensors stay untouched."""

    def __init__(self, sampling: CpuSampling, name: str) -> None:
        self.sampling = sampling
        self.name = name.encode()

    def scope(self, *args: Any, **kwargs: Any) -> AbstractContextManager[None]:
        return self.sampling.function(self.name)

    def invoke(self, original: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
        with self.scope():
            return original(*args, **kwargs)


class CpuMethod(CpuFunction):
    def __init__(self, sampling: CpuSampling, name: str, original: Callable[..., Any]) -> None:
        super().__init__(sampling, name)
        self.original = original

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        return self.invoke(self.original, *args, **kwargs)
