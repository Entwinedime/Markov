"""Profiling 配置规整。"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from profiling.python_probe.trace_sim_probe.schema import HICACHE_FACT_CONSUMERS, runtime_probe_names


KNOWN_CHANNELS = {"torch", "python_probe", "ld_preload"}
PYTHON_PROBE_DIAGNOSTICS = frozenset({"off", "timing"})


@dataclass(frozen=True)
class ProfilingRuntimeConfig:
    """一次 profiling 运行的采集层配置。"""

    enabled: bool
    channels: tuple[str, ...]
    channel_options: dict[str, dict[str, Any]]
    python_consumers: tuple[str, ...]
    python_diagnostics: str
    debug: bool
    post_workload_drain_sec: float
    python_probe_flush_interval_sec: float

    def output_path(self, run_dir: Path, channel: str) -> Path:
        """Resolve the configured torch directory or hook filename prefix relative to the run."""

        key, default = {
            "torch": ("output_dir", "trace/torch"),
            "ld_preload": ("trace_output", "trace/ld_preload/cpu_trace.json"),
        }[channel]
        path = Path(self.channel_options[channel].get(key, default)).expanduser()
        return run_dir / path

    def to_manifest_fragment(self) -> dict[str, Any]:
        """生成写入 profile manifest 的采集配置摘要。"""

        return {
            "enabled": self.enabled,
            "channels_enabled": list(self.channels),
            "python_consumers": list(self.python_consumers),
            "python_diagnostics": self.python_diagnostics,
            "python_runtime_probes": list(runtime_probe_names(self.python_consumers, self.python_diagnostics))
            if self.enabled and "python_probe" in self.channels
            else [],
            "debug": self.debug,
            "capture_tail_contract": {
                "post_workload_drain_sec": self.post_workload_drain_sec,
                "python_probe_flush_interval_sec": self.python_probe_flush_interval_sec,
                "semantic_lifecycle_closure_required_downstream": "hicache_input_contract" in self.python_consumers,
            },
        }


def normalize_profiling_config(cfg: dict[str, Any]) -> ProfilingRuntimeConfig:
    """规整 profiling 配置。

    当前主线把采集渠道统一放到 `profiling` 下。`profiling.torch` 描述 SGLang
    torch profiler，`profiling.python_probe` 描述 Python 侧插桩，
    `profiling.ld_preload` 描述 LD_PRELOAD。
    """

    profiling = cfg.get("profiling") or {}
    if not isinstance(profiling, dict):
        raise TypeError("profiling must be an object")
    enabled = _as_bool(profiling.get("enabled"), default=True)

    # Validate external channel objects once; consumers treat these options as read-only.
    channel_options = {}
    for channel in ("torch", "python_probe", "ld_preload"):
        options = profiling.get(channel)
        if options is None:
            options = {}
        if not isinstance(options, dict):
            raise TypeError(f"profiling.{channel} must be an object")
        channel_options[channel] = options

    channels = _normalize_channels(profiling.get("channels"), channel_options)
    python_probe_cfg = channel_options["python_probe"]

    if "python_probe" in channels:
        python_consumers = _parse_python_consumers(python_probe_cfg.get("consumers"))
        python_diagnostics = _parse_python_diagnostics(python_probe_cfg.get("diagnostics"))
    else:
        python_consumers = ()
        python_diagnostics = "off"

    # Retain asynchronous tail evidence without extending the formal workload window.
    drain_sec = float(cfg.get("post_workload_drain_sec", 0))
    if drain_sec < 0:
        raise ValueError("post_workload_drain_sec must be non-negative")
    try:
        flush_interval_sec = float(python_probe_cfg.get("flush_interval_sec", 0))
    except (TypeError, ValueError) as error:
        raise ValueError("profiling.python_probe.flush_interval_sec must be numeric") from error
    if flush_interval_sec < 0:
        raise ValueError("profiling.python_probe.flush_interval_sec must be non-negative")
    if flush_interval_sec > 0 and drain_sec < flush_interval_sec:
        raise ValueError(
            "post_workload_drain_sec must be at least profiling.python_probe.flush_interval_sec "
            "when periodic probe flushing is enabled"
        )

    return ProfilingRuntimeConfig(
        enabled=enabled,
        channels=channels,
        channel_options=channel_options,
        python_consumers=python_consumers,
        python_diagnostics=python_diagnostics,
        debug=_as_bool(profiling.get("debug", cfg.get("debug")), default=False),
        post_workload_drain_sec=drain_sec,
        python_probe_flush_interval_sec=flush_interval_sec,
    )


def _normalize_channels(value: Any, options: dict[str, dict[str, Any]]) -> tuple[str, ...]:
    """规整采集 channel 列表。

    未显式配置时按各 channel 的 enabled 字段推断，仍只允许当前主线认可的三类 channel。
    显式 `channels: []` 表示本次 run 不启用任何采集通道，用于 forced-token
    capture 这类只需要 workload 产物、不需要 trace 的执行。
    """

    if isinstance(value, list) and not value:
        return ()
    channels = _as_str_tuple(value, default=(), field_name="profiling.channels")
    if not channels:
        channels = tuple(
            channel for channel, config in options.items() if config.get("enabled", channel == "torch")
        ) or ("torch",)
    canonical = tuple(channel.replace("-", "_").lower() for channel in channels)
    unknown = [channel for channel in canonical if channel not in KNOWN_CHANNELS]
    if unknown:
        raise ValueError(f"unknown profiling channel: {unknown}")
    return _unique(canonical)


def _parse_python_consumers(raw: Any) -> tuple[str, ...]:
    """解析本次 Python probe 请求的 consumer 列表。"""

    consumers = _as_str_tuple(raw, default=(), field_name="profiling.python_probe.consumers")
    if not consumers:
        raise ValueError("profiling.python_probe.consumers must list at least one consumer")
    unknown = [consumer for consumer in consumers if consumer not in HICACHE_FACT_CONSUMERS]
    if unknown:
        raise ValueError(f"unknown profiling.python_probe.consumers: {unknown}")
    return _unique(consumers)


def _parse_python_diagnostics(raw: Any) -> str:
    """Parse the single Python-probe diagnostics policy."""

    if raw is None:
        return "off"
    if not isinstance(raw, str):
        raise TypeError("profiling.python_probe.diagnostics must be a string")
    value = raw.strip().lower()
    if value not in PYTHON_PROBE_DIAGNOSTICS:
        allowed = ", ".join(sorted(PYTHON_PROBE_DIAGNOSTICS))
        raise ValueError(f"profiling.python_probe.diagnostics must be one of: {allowed}")
    return value


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
