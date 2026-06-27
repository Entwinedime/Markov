"""Profiling 配置规整。"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from profiling.python_probe.trace_sim_probe.schema import HICACHE_FACT_CONSUMERS


DEFAULT_PYTHON_PROBES = ("generic_callable",)
KNOWN_CHANNELS = {"torch", "python_probe", "ld_preload"}


@dataclass(frozen=True)
class ProfilingRuntimeConfig:
    """一次 profiling 运行的采集层配置。"""

    enabled: bool
    channels: tuple[str, ...]
    python_probes: tuple[str, ...]
    python_consumers: tuple[str, ...]
    python_target_catalog: str | None
    debug: bool

    def to_manifest_fragment(self) -> dict[str, Any]:
        """生成写入 profile manifest 的采集配置摘要。"""

        return {
            "enabled": self.enabled,
            "channels_enabled": list(self.channels),
            "python_probes_enabled": list(self.python_probes),
            "python_consumers": list(self.python_consumers),
            "python_target_catalog": self.python_target_catalog,
            "debug": self.debug,
        }


def normalize_profiling_config(cfg: dict[str, Any]) -> ProfilingRuntimeConfig:
    """规整 profiling 配置。

    新主线把采集渠道统一放到 `profiling` 下。`profiling.torch` 描述 SGLang
    torch profiler，`profiling.python_probe` 描述 Python 侧插桩，
    `profiling.ld_preload` 描述 LD_PRELOAD。这里不读取旧顶层兼容字段。
    """

    profiling = cfg.get("profiling") or {}
    if not isinstance(profiling, dict):
        raise TypeError("profiling must be an object")
    enabled = _as_bool(profiling.get("enabled"), default=True)

    channels = _normalize_channels(profiling.get("channels"), cfg)
    python_probe_cfg = profiling.get("python_probe") or {}
    if not isinstance(python_probe_cfg, dict):
        raise TypeError("profiling.python_probe must be an object")

    if "python_probe" in channels:
        _reject_unknown_python_probe_config(python_probe_cfg)
        python_probes = _as_str_tuple(
            python_probe_cfg.get("probes", python_probe_cfg.get("name", profiling.get("probes"))),
            default=DEFAULT_PYTHON_PROBES,
            field_name="profiling.python_probe.probes",
        )
        python_consumers = _parse_python_consumers(python_probe_cfg.get("consumers"))
        catalog = python_probe_cfg.get("target_catalog")
        if catalog is not None and not isinstance(catalog, str):
            raise TypeError("profiling.python_probe.target_catalog must be a string")
        python_target_catalog = catalog
    else:
        python_probes = ()
        python_consumers = ()
        python_target_catalog = None
    return ProfilingRuntimeConfig(
        enabled=enabled,
        channels=channels,
        python_probes=python_probes,
        python_consumers=python_consumers,
        python_target_catalog=python_target_catalog,
        debug=_as_bool(profiling.get("debug", cfg.get("debug")), default=False),
    )


def _normalize_channels(value: Any, cfg: dict[str, Any]) -> tuple[str, ...]:
    """规整采集 channel 列表。

    未显式配置时按各 channel 的 enabled 字段推断，仍只允许当前主线认可的三类 channel。
    显式 `channels: []` 表示本次 run 不启用任何采集通道，用于 forced-token
    capture 这类只需要 workload 产物、不需要 trace 的执行。
    """

    if isinstance(value, list) and not value:
        return ()
    channels = _as_str_tuple(value, default=(), field_name="profiling.channels")
    if not channels:
        inferred: list[str] = []
        profiling = cfg.get("profiling") if isinstance(cfg.get("profiling"), dict) else {}
        torch_cfg = _channel_cfg(cfg, "torch")
        python_probe_cfg = profiling.get("python_probe") if isinstance(profiling.get("python_probe"), dict) else {}
        ld_preload_cfg = _channel_cfg(cfg, "ld_preload")
        if torch_cfg.get("enabled", True):
            inferred.append("torch")
        if python_probe_cfg.get("enabled", False):
            inferred.append("python_probe")
        if ld_preload_cfg.get("enabled", False):
            inferred.append("ld_preload")
        channels = tuple(inferred) or ("torch",)
    canonical = tuple(channel.replace("-", "_").lower() for channel in channels)
    unknown = [channel for channel in canonical if channel not in KNOWN_CHANNELS]
    if unknown:
        raise ValueError(f"unknown profiling channel: {unknown}")
    return _unique(canonical)


def _channel_cfg(cfg: dict[str, Any], profiling_key: str) -> dict[str, Any]:
    """读取 `profiling.<channel>` 对象；缺失或类型错误时返回空配置。"""

    profiling = cfg.get("profiling") if isinstance(cfg.get("profiling"), dict) else {}
    current = profiling.get(profiling_key)
    if isinstance(current, dict):
        return current
    return {}


def _reject_unknown_python_probe_config(python_probe_cfg: dict[str, Any]) -> None:
    """Reject python_probe config outside the active target-catalog contract."""

    allowed = {
        "enabled",
        "name",
        "probes",
        "consumers",
        "target_catalog",
        "flush_every",
        "flush_interval_events",
        "internal_hooks",
    }
    unknown = sorted(set(python_probe_cfg) - allowed)
    if unknown:
        raise ValueError(f"unknown profiling.python_probe fields: {unknown}")


def _parse_python_consumers(raw: Any) -> tuple[str, ...]:
    """解析本次 Python probe 请求的 consumer 列表。"""

    consumers = _as_str_tuple(raw, default=(), field_name="profiling.python_probe.consumers")
    if not consumers:
        raise ValueError("profiling.python_probe.consumers must list at least one consumer")
    unknown = [consumer for consumer in consumers if consumer not in HICACHE_FACT_CONSUMERS]
    if unknown:
        raise ValueError(f"unknown profiling.python_probe.consumers: {unknown}")
    return _unique(consumers)


def _as_str_tuple(value: Any, *, default: tuple[str, ...], field_name: str) -> tuple[str, ...]:
    """把字符串或字符串数组规整为 tuple。"""

    if value is None:
        return default
    if isinstance(value, str):
        return (value,)
    if isinstance(value, list) and all(isinstance(item, str) for item in value):
        return tuple(value)
    raise TypeError(f"{field_name} must be a string or an array of strings")


def _as_bool(value: Any, *, default: bool) -> bool:
    """只接受 JSON boolean，避免字符串布尔值在配置里被静默解释。"""

    if value is None:
        return default
    if isinstance(value, bool):
        return value
    raise TypeError("boolean config values must use true/false")


def _unique(values: tuple[str, ...]) -> tuple[str, ...]:
    """保持顺序去重。"""

    result: list[str] = []
    seen: set[str] = set()
    for value in values:
        if value not in seen:
            seen.add(value)
            result.append(value)
    return tuple(result)
