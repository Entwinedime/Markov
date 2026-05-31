from __future__ import annotations

import functools
from typing import Any, Callable, Optional


PATCH_MARKER = "__trace_sim_probe_wrapped__"


def safe_len(value: Any) -> Optional[int]:
    if value is None:
        return None
    try:
        if hasattr(value, "numel"):
            return int(value.numel())
        return len(value)
    except Exception:
        return None


def safe_getattr(obj: Any, name: str, default: Any = None) -> Any:
    try:
        return getattr(obj, name, default)
    except Exception:
        return default


def compact_id(value: Any) -> str:
    obj_id = safe_getattr(value, "id", None)
    if obj_id is not None:
        return str(obj_id)
    return hex(id(value))


def wrap_function(module: Any, function_name: str, wrapper_factory: Callable[[Callable[..., Any]], Callable[..., Any]]) -> bool:
    fn = safe_getattr(module, function_name)
    if fn is None or safe_getattr(fn, PATCH_MARKER, False):
        return False
    wrapped = wrapper_factory(fn)
    setattr(wrapped, PATCH_MARKER, True)
    setattr(module, function_name, wrapped)
    return True


def wrap_method(cls: Any, method_name: str, wrapper_factory: Callable[[Callable[..., Any]], Callable[..., Any]]) -> bool:
    method = safe_getattr(cls, method_name)
    if method is None or safe_getattr(method, PATCH_MARKER, False):
        return False

    @functools.wraps(method)
    def wrapped(*args: Any, **kwargs: Any) -> Any:
        return wrapper_factory(method)(*args, **kwargs)

    setattr(wrapped, PATCH_MARKER, True)
    setattr(cls, method_name, wrapped)
    return True
