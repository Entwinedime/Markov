"""Profiling 配置规整。"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any


DEFAULT_PYTHON_PROBES = ("generic_callable",)
KNOWN_CHANNELS = {"torch", "python_probe", "ld_preload"}
KNOWN_FACT_CLASSES = {"invariant_state", "timing_observation", "source_actual", "oracle_state", "debug_quality"}


@dataclass(frozen=True)
class ProfilingRuntimeConfig:
    """一次 profiling 运行的采集层配置。"""

    enabled: bool
    channels: tuple[str, ...]
    python_probes: tuple[str, ...]
    python_targets: tuple[dict[str, Any], ...]
    python_state_trace_enabled: bool
    debug: bool

    def to_manifest_fragment(self) -> dict[str, Any]:
        return {
            "enabled": self.enabled,
            "channels_enabled": list(self.channels),
            "python_probes_enabled": list(self.python_probes),
            "python_targets": list(self.python_targets),
            "python_state_trace_enabled": self.python_state_trace_enabled,
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
        python_probes = _as_str_tuple(
            python_probe_cfg.get("probes", profiling.get("probes")),
            default=DEFAULT_PYTHON_PROBES,
            field_name="profiling.python_probe.probes",
        )
        python_targets = _parse_python_targets(python_probe_cfg.get("targets", []))
        state_trace = python_probe_cfg.get("state_trace") if isinstance(python_probe_cfg.get("state_trace"), dict) else {}
        python_state_trace_enabled = _as_bool(state_trace.get("enabled"), default=False)
    else:
        python_probes = ()
        python_targets = ()
        python_state_trace_enabled = False
    return ProfilingRuntimeConfig(
        enabled=enabled,
        channels=channels,
        python_probes=python_probes,
        python_targets=python_targets,
        python_state_trace_enabled=python_state_trace_enabled,
        debug=_as_bool(profiling.get("debug", cfg.get("debug")), default=False),
    )


def _normalize_channels(value: Any, cfg: dict[str, Any]) -> tuple[str, ...]:
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
    profiling = cfg.get("profiling") if isinstance(cfg.get("profiling"), dict) else {}
    current = profiling.get(profiling_key)
    if isinstance(current, dict):
        return current
    return {}


def _parse_python_targets(raw: Any) -> tuple[dict[str, Any], ...]:
    if raw is None:
        return ()
    if not isinstance(raw, list):
        raise TypeError("profiling.python_probe.targets must be an array")
    result: list[dict[str, Any]] = []
    for index, item in enumerate(raw):
        if not isinstance(item, dict):
            raise TypeError(f"profiling.python_probe.targets[{index}] must be an object")
        target_id = item.get("id")
        target = item.get("target")
        module = item.get("module")
        if not isinstance(target_id, str) or not target_id:
            raise ValueError(f"profiling.python_probe.targets[{index}].id must be a non-empty string")
        if not isinstance(target, str) or not target:
            raise ValueError(f"profiling.python_probe.targets[{index}].target must be a non-empty string")
        if not isinstance(module, str) or not module:
            raise ValueError(f"profiling.python_probe.targets[{index}].module must be a non-empty string")
        _validate_python_target_fact(item, index)
        result.append(dict(item))
    return tuple(result)


def _validate_python_target_fact(item: dict[str, Any], index: int) -> None:
    fact = item.get("fact")
    prefix = f"profiling.python_probe.targets[{index}].fact"
    if not isinstance(fact, dict):
        raise ValueError(f"{prefix} must be an object")
    fact_class = fact.get("class")
    if not isinstance(fact_class, str) or not fact_class:
        raise ValueError(f"{prefix}.class must be a non-empty string")
    if fact_class not in KNOWN_FACT_CLASSES:
        raise ValueError(f"{prefix}.class must be one of {sorted(KNOWN_FACT_CLASSES)}")
    role = fact.get("role")
    if not isinstance(role, str) or not role:
        raise ValueError(f"{prefix}.role must be a non-empty string")
    if fact.get("granularity") != "atomic":
        raise ValueError(f"{prefix}.granularity must be 'atomic'")
    if not isinstance(fact.get("model_input"), bool):
        raise ValueError(f"{prefix}.model_input must be true or false")
    if not isinstance(fact.get("dag_input"), bool):
        raise ValueError(f"{prefix}.dag_input must be true or false")


def _as_str_tuple(value: Any, *, default: tuple[str, ...], field_name: str) -> tuple[str, ...]:
    if value is None:
        return default
    if isinstance(value, str):
        return (value,)
    if isinstance(value, list) and all(isinstance(item, str) for item in value):
        return tuple(value)
    raise TypeError(f"{field_name} must be a string or an array of strings")


def _as_bool(value: Any, *, default: bool) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    raise TypeError("boolean config values must use true/false")


def _unique(values: tuple[str, ...]) -> tuple[str, ...]:
    result: list[str] = []
    seen: set[str] = set()
    for value in values:
        if value not in seen:
            seen.add(value)
            result.append(value)
    return tuple(result)
