"""Optional host preparation intervals, not HiCache facts or device costs.

Triton preparation may load a disk cache entry or compile a new variant. With
async compilation the interval ends at submission return, not kernel readiness.
No tensor contents, cache keys, snapshots or per-launch records are collected.
"""

from __future__ import annotations

import functools
import inspect
from types import ModuleType

from trace_sim_probe.patching import PATCH_MARKER
from trace_sim_probe.writer import get_writer


_TARGETS = {
    "triton.runtime.jit": ("JITFunction", "_do_compile", "runtime.triton.prepare"),
    "triton.compiler.compiler": ("CompiledKernel", "_init_handles", "runtime.triton.load"),
}
TARGET_MODULES = tuple(_TARGETS)


def install(module: ModuleType) -> None:
    """Wrap already imported methods; do not import Triton or initialize devices."""

    class_name, method, event = _TARGETS[module.__name__]
    owner = getattr(module, class_name, None)
    original = getattr(owner, method, None)
    if original is None or getattr(original, PATCH_MARKER, False):
        return
    parameters = inspect.signature(original)

    @functools.wraps(original)
    def measured(instance, *args, **kwargs):
        if method == "_init_handles" and instance.module is not None:
            return original(instance, *args, **kwargs)

        arguments = parameters.bind(instance, *args, **kwargs).arguments
        source = getattr(instance, "src", None)
        attributes = arguments.get("attrs", getattr(source, "attrs", None))
        fields = {
            "kernel": instance.fn.__qualname__ if method == "_do_compile" else instance.name,
            "signature": arguments.get("signature", getattr(source, "signature", {})),
            "constants": arguments.get("constants", getattr(source, "constants", {})),
            "argument_properties": getattr(attributes, "arg_properties", {}),
            "status": "raised",
        }
        writer = get_writer()
        start = writer.now_us()
        try:
            result = original(instance, *args, **kwargs)
            fields["status"] = "returned"
            return result
        finally:
            end = writer.now_us()
            writer.duration_event(event, start, end, "runtime_diagnostic", fields)

    setattr(measured, PATCH_MARKER, True)
    setattr(owner, method, measured)
