"""Python probe bootstrap。"""

from __future__ import annotations

import builtins
import functools
import importlib
import os
import sys
import threading
from types import ModuleType

from trace_sim_probe.writer import _truthy
from trace_sim_probe.schema import runtime_probe_names
from trace_sim_probe.probes.values import configure_source_extractor


_INSTALLED = False
_LOCK = threading.Lock()
_ORIGINAL_IMPORT = builtins.__import__
_ORIGINAL_IMPORT_MODULE = importlib.import_module
_IMPORT_GUARD = threading.local()
_PROBES: tuple[ModuleType, ...] | None = None


def _probes() -> tuple[ModuleType, ...]:
    """Load consumer-required observations and optional runtime diagnostics once."""

    global _PROBES
    if _PROBES is None:
        # Bootstrap is the composition boundary. Common recording/value
        # readers do not import SGLang; bind its scalar reader before wrapping.
        framework_package = "trace_sim_probe.adapters.frameworks.sglang"
        sources = importlib.import_module(framework_package + ".hicache.sources")
        configure_source_extractor(sources.extract_source)

        probes = [importlib.import_module("trace_sim_probe.probes.generic_callable")]
        consumers = tuple(os.environ.get("TRACE_SIM_PYTHON_PROBE_CONSUMERS", "").split(","))
        diagnostics = os.environ.get("TRACE_SIM_PYTHON_PROBE_DIAGNOSTICS", "off")
        probes.extend(
            importlib.import_module(framework_package + "." + name)
            for name in runtime_probe_names(consumers, diagnostics)
        )
        _PROBES = tuple(probes)
    return _PROBES


def _apply_probe_to_loaded_modules(probe: ModuleType) -> None:
    """对当前已加载的目标模块立即安装 probe。"""

    targets: tuple[str, ...] = getattr(probe, "TARGET_MODULES", ())
    for target in targets:
        module = sys.modules.get(target)
        if module is not None:
            probe.install(module)


@functools.cache
def _matching_probes(module_name: str) -> tuple[ModuleType, ...]:
    """Probe targets are fixed at startup; repeated imports need no new scan."""
    matches = []
    for probe in _probes():
        targets: tuple[str, ...] = getattr(probe, "TARGET_MODULES", ())
        if any(
            module_name == target or module_name.startswith(target + ".") or target.startswith(module_name + ".")
            for target in targets
        ):
            matches.append(probe)
    return tuple(matches)


def _post_import_apply(module_name: str) -> None:
    """Recheck installation even when the module-name match was cached."""
    for probe in _matching_probes(module_name):
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


def _import_module_hook(name: str, package: str | None = None) -> ModuleType:
    """Observe completed dynamic imports used by the framework model registry.

    importlib.import_module bypasses builtins.__import__ for the requested
    module. Its nested imports can expose a partially defined model, so the
    final installation must also run after the dynamic import returns.
    """
    was_active = getattr(_IMPORT_GUARD, "active", False)
    _IMPORT_GUARD.active = True
    try:
        module = _ORIGINAL_IMPORT_MODULE(name, package)
        # Model registries also load modules inside an ordinary import. Do
        # not skip that completed module merely because its parent is loading.
        _post_import_apply(module.__name__)
        return module
    finally:
        _IMPORT_GUARD.active = was_active


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
        importlib.import_module = _import_module_hook

    for probe in probes:
        _apply_probe_to_loaded_modules(probe)
