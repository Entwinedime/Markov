"""Profiling 主线模块。

Profiling 只负责采集事实和生成采集摘要。Python probe 与 LD_PRELOAD 是两条
独立采集通道：Python probe 由 Python 配置控制，LD_PRELOAD 由 hook 实现硬编码
采集逻辑和 runner 的 `profiling.ld_preload` 配置控制。
"""

from .config import ProfilingRuntimeConfig, normalize_profiling_config
from .manifest import build_profile_manifest

__all__ = [
    "ProfilingRuntimeConfig",
    "build_profile_manifest",
    "normalize_profiling_config",
]
