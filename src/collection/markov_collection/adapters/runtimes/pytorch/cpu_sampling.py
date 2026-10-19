"""Observe operation and explicit native boundaries without starting a profiler."""

import ctypes


def observe_operators() -> None:
    native = ctypes.CDLL(None)
    native.markov_cpu_observe_pytorch.argtypes = []
    native.markov_cpu_observe_pytorch.restype = None
    native.markov_cpu_observe_pytorch()
