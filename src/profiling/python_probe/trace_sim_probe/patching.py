"""Python probe patch 工具。"""

from __future__ import annotations

import functools
from typing import Any, Callable


PATCH_MARKER = "__trace_sim_probe_wrapped__"


def safe_getattr(obj: Any, name: str, default: Any = None) -> Any:
    """读取属性时吞掉目标框架自定义 descriptor 可能抛出的异常。"""

    try:
        return getattr(obj, name, default)
    except Exception:
        return default


def safe_len(value: Any) -> int | None:
    """读取容器长度或 tensor numel，失败时返回 None。"""

    try:
        if value is None:
            return None
        if hasattr(value, "numel"):
            return int(value.numel())
        return len(value)
    except Exception:
        return None


def compact_id(value: Any) -> str:
    """生成稳定可读的对象标识；优先使用对象自身 id 字段。"""

    obj_id = safe_getattr(value, "id", None)
    if obj_id is not None:
        return str(obj_id)
    return hex(id(value))


def wrap_function(
    module: Any,
    function_name: str,
    wrapper_factory: Callable[[Callable[..., Any]], Callable[..., Any]],
) -> bool:
    """给模块函数安装 wrapper，已安装过时保持幂等。"""

    fn = safe_getattr(module, function_name)
    if fn is None or safe_getattr(fn, PATCH_MARKER, False):
        return False
    wrapped = wrapper_factory(fn)
    setattr(wrapped, PATCH_MARKER, True)
    setattr(module, function_name, wrapped)
    return True


def wrap_method(
    cls: Any,
    method_name: str,
    wrapper_factory: Callable[[Callable[..., Any]], Callable[..., Any]],
) -> bool:
    """给类方法安装 wrapper，已安装过时保持幂等。"""

    method = safe_getattr(cls, method_name)
    if method is None or safe_getattr(method, PATCH_MARKER, False):
        return False

    @functools.wraps(method)
    def wrapped(*args: Any, **kwargs: Any) -> Any:
        return wrapper_factory(method)(*args, **kwargs)

    setattr(wrapped, PATCH_MARKER, True)
    setattr(cls, method_name, wrapped)
    return True
