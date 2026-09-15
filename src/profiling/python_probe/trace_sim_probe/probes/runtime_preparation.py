"""Optional host preparation intervals, not HiCache facts or device costs.

Triton preparation may load a disk cache entry or compile a new variant. With
async compilation the interval ends at submission return, not kernel readiness.
No tensor contents, cache keys, snapshots or per-launch records are collected.
"""

from __future__ import annotations

import functools
import inspect
import sys
from contextvars import ContextVar
from types import ModuleType

from trace_sim_probe.patching import PATCH_MARKER
from trace_sim_probe.writer import get_writer


_TARGETS = {
    "triton.runtime.jit": ("JITFunction", "_do_compile", "runtime.triton.prepare"),
    "triton.compiler.compiler": ("CompiledKernel", "_init_handles", "runtime.triton.load"),
}
TARGET_MODULES = tuple(_TARGETS)
_PREPARATION = ContextVar("triton_preparation", default=None)


def _observe_ir_entry(module):
    owner = getattr(module, "ASTSource", None)
    original = getattr(owner, "make_ir", None)
    if original is None or getattr(original, PATCH_MARKER, False):
        return

    @functools.wraps(original)
    def observed(source, *args, **kwargs):
        current = _PREPARATION.get()
        if current is not None:
            instance, fields = current
            if source.fn is instance and fields["execution_mode"] == "sync":
                fields["path"] = "compiled"
        return original(source, *args, **kwargs)

    setattr(observed, PATCH_MARKER, True)
    owner.make_ir = observed


def install(module: ModuleType) -> None:
    """Wrap already imported methods; do not import Triton or initialize devices."""

    if module.__name__ == "triton.compiler.compiler":
        _observe_ir_entry(module)
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
        token = None
        if method == "_do_compile":
            # Ascend installs its own IR wrapper lazily on first compilation.
            # Observe the current entry again on a later preparation call.
            _observe_ir_entry(sys.modules.get("triton.compiler.compiler"))
            active_mode = getattr(getattr(module, "_async_compile", None), "active_mode", None)
            mode = "unknown" if active_mode is None else "sync" if active_mode.get() is None else "async"
            fields.update(execution_mode=mode, path="async_submit" if mode == "async" else "unknown")
            token = _PREPARATION.set((instance, fields))
        writer = get_writer()
        start = writer.now_us()
        try:
            result = original(instance, *args, **kwargs)
            fields["status"] = "returned"
            if method == "_do_compile" and fields["execution_mode"] == "sync" and fields["path"] == "unknown":
                source = getattr(result, "src", None)
                ir_entry = getattr(type(source), "make_ir", None)
                # Only the observed AST path and a matching returned kernel prove
                # disk reuse. Hooks, async submission and older APIs stay unknown.
                if getattr(ir_entry, PATCH_MARKER, False) and getattr(source, "fn", None) is instance:
                    fields["path"] = "disk_cache"
            return result
        finally:
            end = writer.now_us()
            if token is not None:
                _PREPARATION.reset(token)
            writer.duration_event(event, start, end, "runtime_diagnostic", fields)

    setattr(measured, PATCH_MARKER, True)
    setattr(owner, method, measured)
