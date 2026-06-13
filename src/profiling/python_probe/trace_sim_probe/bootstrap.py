"""Python probe bootstrap。"""

from __future__ import annotations

import builtins
import importlib
import os
import sys
import threading
from types import ModuleType
from typing import Iterable

from trace_sim_probe.writer import probe_debug_enabled


_INSTALLED = False
_LOCK = threading.Lock()
_ORIGINAL_IMPORT = builtins.__import__
_IMPORT_GUARD = threading.local()


def _truthy(value: str | None) -> bool:
    """解析 probe 启用环境变量。"""

    return value is not None and value.lower() not in ("", "0", "false", "no", "off")


def _selected_probe_names() -> list[str]:
    """读取 TRACE_SIM_PYTHON_PROBES 中选择的 probe 名称。"""

    raw = os.environ.get("TRACE_SIM_PYTHON_PROBES", "generic_callable")
    return [part.strip() for part in raw.split(",") if part.strip()]


def _load_probe(name: str):
    """按短名称或完整模块名加载 probe。"""

    module_name = {
        "generic_callable": "trace_sim_probe.probes.generic_callable",
        "sglang.hicache": "trace_sim_probe.probes.sglang_hicache_callable",
        "sglang_hicache": "trace_sim_probe.probes.sglang_hicache_callable",
        "sglang_hicache_callable": "trace_sim_probe.probes.sglang_hicache_callable",
        "sglang.kvcacheio": "trace_sim_probe.probes.sglang_kvcacheio",
    }.get(name, name)
    return importlib.import_module(module_name)


def _iter_selected_probes():
    """迭代已选择且成功加载的 probe；失败只在 debug 模式输出 stderr。"""

    for name in _selected_probe_names():
        try:
            yield _load_probe(name)
        except Exception as exc:
            if probe_debug_enabled():
                print(f"[trace_sim_probe] failed to load probe {name}: {exc}", file=sys.stderr)


def _apply_probe_to_loaded_modules(probe) -> None:
    """对当前已加载的目标模块立即安装 probe。"""

    targets: Iterable[str] = getattr(probe, "TARGET_MODULES", ())
    for target in targets:
        module = sys.modules.get(target)
        if module is not None:
            _safe_install(probe, module)


def _safe_install(probe, module: ModuleType) -> None:
    """安装 probe 时隔离异常，避免插桩破坏被测进程启动。"""

    try:
        probe.install(module)
    except Exception as exc:
        if probe_debug_enabled():
            print(f"[trace_sim_probe] probe install failed for {module.__name__}: {exc}", file=sys.stderr)


def _post_import_apply(module_name: str) -> None:
    """一次 import 完成后，对相关目标模块补装 probe。"""

    for probe in _iter_selected_probes():
        targets: Iterable[str] = getattr(probe, "TARGET_MODULES", ())
        for target in targets:
            if module_name == target or module_name.startswith(target + "."):
                module = sys.modules.get(target)
                if module is not None:
                    _safe_install(probe, module)
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

    try:
        resolved_name = getattr(module, "__name__", name)
        _post_import_apply(resolved_name)
        for item in fromlist or ():
            child = f"{resolved_name}.{item}"
            if child in sys.modules:
                _post_import_apply(child)
    except Exception:
        pass
    return module


def bootstrap() -> None:
    """安装 import hook 并对已加载模块应用 probe。"""

    global _INSTALLED
    if not _truthy(os.environ.get("TRACE_SIM_PYTHON_PROBE")):
        return

    with _LOCK:
        if _INSTALLED:
            return
        _INSTALLED = True
        builtins.__import__ = _import_hook

    for probe in _iter_selected_probes():
        _apply_probe_to_loaded_modules(probe)
