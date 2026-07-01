"""SGLang HiCache callable probe 入口。"""

from __future__ import annotations

from types import ModuleType

from trace_sim_probe.probes import generic_callable as _base

from .sources import register_source_extractors


register_source_extractors()


def install(module: ModuleType) -> None:
    """安装通用 callable probe。"""

    _base.install(module)


TARGET_MODULES = _base.TARGET_MODULES
