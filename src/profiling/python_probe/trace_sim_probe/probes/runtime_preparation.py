"""Host preparation observations for DAG modeling, not HiCache state facts.

Triton preparation may load a disk cache entry or compile a new variant. With
async compilation the interval ends at submission return, not kernel readiness.
No tensor contents, cache keys, snapshots or per-launch records are collected.
"""

from __future__ import annotations

import inspect
import sys
from contextvars import ContextVar
from functools import partial
from types import ModuleType
from typing import Any, Callable

from trace_sim_probe.patching import PATCH_MARKER, install_wrapper
from trace_sim_probe.writer import get_writer


_TARGETS = {
    "triton.runtime.jit": ("JITFunction", "_do_compile", "runtime.triton.prepare"),
    "triton.compiler.compiler": ("CompiledKernel", "_init_handles", "runtime.triton.load"),
}
TARGET_MODULES = tuple(_TARGETS)
_PREPARATION = ContextVar("triton_preparation", default=None)


def _ir_entry(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(source: Any, *args: Any, **kwargs: Any) -> Any:
        current = _PREPARATION.get()
        if current is not None:
            instance, fields = current
            if source.fn is instance and fields["execution_mode"] == "sync":
                fields["path"] = "compiled"
        return original(source, *args, **kwargs)

    return observed


def _preparation(original: Callable[..., Any], module: ModuleType, method: str, event: str) -> Callable[..., Any]:
    parameters = inspect.signature(original)

    def measured(instance: Any, *args: Any, **kwargs: Any) -> Any:
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
            compiler = sys.modules.get("triton.compiler.compiler")
            install_wrapper(getattr(compiler, "ASTSource", None), "make_ir", _ir_entry)
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

    return measured


def install(module: ModuleType) -> None:
    """Wrap already imported methods; do not import Triton or initialize devices."""

    if module.__name__ == "triton.compiler.compiler":
        install_wrapper(getattr(module, "ASTSource", None), "make_ir", _ir_entry)

    class_name, method, event = _TARGETS[module.__name__]
    install_wrapper(
        getattr(module, class_name, None), method, partial(_preparation, module=module, method=method, event=event)
    )
