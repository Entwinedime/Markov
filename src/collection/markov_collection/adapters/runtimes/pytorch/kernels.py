"""Wrap a declared kernel's native launcher without changing its compilation or grid."""

from __future__ import annotations

from typing import Any

from markov_collection.instrumentation.calls import NativeCall


class KernelObservation:
    def __init__(self, values: Any, kernel: Any, call: type[NativeCall]) -> None:
        self.values = values
        self.kernel = kernel
        self.call = call

    def __getitem__(self, grid: Any) -> Any:
        original = self.kernel[grid]
        if self.values.recording.current_scope.get() is None:
            return original

        # Wrap the native launcher after grid selection so compilation remains owned by the kernel.
        return self.call(self.values, self.kernel, original)

    def __getattr__(self, name: str) -> Any:
        return getattr(self.kernel, name)
