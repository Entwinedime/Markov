"""Python probe bootstrap。"""

from __future__ import annotations

import builtins
import importlib
import os
import sys
import threading
from types import ModuleType

from trace_sim_probe.writer import probe_debug_enabled


_INSTALLED = False
_LOCK = threading.Lock()
_ORIGINAL_IMPORT = builtins.__import__
_IMPORT_GUARD = threading.local()
_PROBES: tuple[ModuleType, ...] | None = None


def _truthy(value: str | None) -> bool:
    """解析 probe 启用环境变量。"""

    return value is not None and value.lower() not in ("", "0", "false", "no", "off")


def _probes() -> tuple[ModuleType, ...]:
    """Load semantic probes and explicitly enabled runtime diagnostics once."""

    global _PROBES
    if _PROBES is None:
        try:
            probes = [importlib.import_module("trace_sim_probe.probes.hicache.callable")]
            if os.environ.get("TRACE_SIM_PYTHON_PROBE_DIAGNOSTICS", "off") in {"timing", "full"}:
                probes.append(importlib.import_module("trace_sim_probe.probes.runtime_preparation"))
            _PROBES = tuple(probes)
        except Exception as exc:
            if probe_debug_enabled():
                print(f"[trace_sim_probe] failed to load probes: {exc}", file=sys.stderr)
            raise
    return _PROBES


def _apply_probe_to_loaded_modules(probe) -> None:
    """对当前已加载的目标模块立即安装 probe。"""

    targets: tuple[str, ...] = getattr(probe, "TARGET_MODULES", ())
    for target in targets:
        module = sys.modules.get(target)
        if module is not None:
            _safe_install(probe, module)


def _safe_install(probe, module: ModuleType) -> None:
    """安装 probe；profiling 合同错误必须直接暴露。"""

    try:
        probe.install(module)
    except Exception as exc:
        if probe_debug_enabled():
            print(f"[trace_sim_probe] probe install failed for {module.__name__}: {exc}", file=sys.stderr)
        raise


def _post_import_apply(module_name: str) -> None:
    """一次 import 完成后，对相关目标模块补装 probe。"""

    for probe in _probes():
        targets: tuple[str, ...] = getattr(probe, "TARGET_MODULES", ())
        if any(
            module_name == target or module_name.startswith(target + ".") or target.startswith(module_name + ".")
            for target in targets
        ):
            _apply_probe_to_loaded_modules(probe)


def _import_hook(name, globals=None, locals=None, fromlist=(), level=0):
    """包装 Python import，在目标模块加载后安装 probe。"""

    if getattr(_IMPORT_GUARD, "active", False):
        return _ORIGINAL_IMPORT(name, globals, locals, fromlist, level)
    _IMPORT_GUARD.active = True
    try:
        module = _ORIGINAL_IMPORT(name, globals, locals, fromlist, level)
    finally:
        _IMPORT_GUARD.active = False

    resolved_name = getattr(module, "__name__", name)
    _post_import_apply(resolved_name)
    for item in fromlist or ():
        child = f"{resolved_name}.{item}"
        if child in sys.modules:
            _post_import_apply(child)
    return module


def bootstrap() -> None:
    """安装 import hook 并对已加载模块应用 probe。"""

    global _INSTALLED
    if not _truthy(os.environ.get("TRACE_SIM_PYTHON_PROBE")):
        return

    with _LOCK:
        if _INSTALLED:
            return
        probes = _probes()
        _INSTALLED = True
        builtins.__import__ = _import_hook

    for probe in probes:
        _apply_probe_to_loaded_modules(probe)
