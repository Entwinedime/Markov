"""通用 Python callable probe。

该 probe 只处理 Python 侧配置，不参与 LD_PRELOAD。目标配置来自
`TRACE_SIM_PYTHON_PROBE_TARGETS`，格式为 JSON 数组。
"""

from __future__ import annotations

import functools
import inspect
import json
import os
import sys
import time
from types import ModuleType
from typing import Any, Callable

from trace_sim_probe.patching import PATCH_MARKER
from trace_sim_probe.schema import FieldSpec, TargetSpec, parse_target
from trace_sim_probe.writer import get_writer, probe_debug_enabled
from .thread_timing import (
    _thread_timing_delta,
    _thread_timing_snapshot,
)
from .values import extract_value, hicache_source_extractor


@functools.cache
def _load_targets() -> dict[str, dict[str, list[TargetSpec]]]:
    """Parse environment input once and group targets by module and callable."""

    raw = os.environ.get("TRACE_SIM_PYTHON_PROBE_TARGETS", "[]")
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ValueError("TRACE_SIM_PYTHON_PROBE_TARGETS must be a JSON array") from exc
    if not isinstance(data, list):
        raise ValueError("TRACE_SIM_PYTHON_PROBE_TARGETS must be a JSON array")

    targets: dict[str, dict[str, list[TargetSpec]]] = {}
    for index, item in enumerate(data):
        if not isinstance(item, dict):
            raise ValueError(f"TRACE_SIM_PYTHON_PROBE_TARGETS[{index}] must be an object")
        target = parse_target(item)
        targets.setdefault(target.module_name, {}).setdefault(target.target, []).append(target)
    return targets


def install(module: ModuleType) -> None:
    """在模块加载后安装该模块匹配的 callable wrapper。"""

    for targets in _load_targets().get(module.__name__, {}).values():
        target = targets[0]
        resolved = _resolve_target(module, target)
        if resolved is None:
            continue
        owner, attr_name, original = resolved
        if original is None or getattr(original, PATCH_MARKER, False) or not callable(original):
            continue
        wrapped = _wrap_callable(tuple(targets), original)
        setattr(wrapped, PATCH_MARKER, True)
        setattr(owner, attr_name, wrapped)
        if probe_debug_enabled():
            target_ids = ",".join(item.id for item in targets)
            print(f"[trace_sim_probe] patched {target.target} targets={target_ids}", file=sys.stderr)


def _resolve_target(module: ModuleType, target: TargetSpec) -> tuple[Any, str, Any] | None:
    """解析模块内 callable。

    返回 owner 对象、owner 上的属性名和原始 callable。owner 可能是 module，也可能
    是类对象；这样同一个 generic probe 可以覆盖模块函数和类方法。
    """

    parts = [part for part in target.target.split(".") if part]
    if not parts:
        return None
    owner: Any = module
    for part in parts[:-1]:
        owner = getattr(owner, part, None)
        if owner is None:
            return None
    attr_name = parts[-1]
    original = getattr(owner, attr_name, None)
    return owner, attr_name, original


def _wrap_callable(targets: tuple[TargetSpec, ...], fn: Callable[..., Any]) -> Callable[..., Any]:
    """包装同步或异步 callable，并按 target phase 配置发事件。"""

    hicache_source_extractor()
    # The installed callable is fixed; only its argument values change per call.
    try:
        argument_spec = inspect.signature(fn)
    except (TypeError, ValueError):
        argument_spec = None
    capture_thread_timing = any(target.capture_thread_timing for target in targets)

    if inspect.iscoroutinefunction(fn):

        @functools.wraps(fn)
        async def async_wrapped(*args: Any, **kwargs: Any) -> Any:
            timing_started = _thread_timing_snapshot() if capture_thread_timing else None
            started = get_writer().now_us()
            _emit_targets(targets, argument_spec, args, kwargs, None, "start", started, started, None)
            result = await fn(*args, **kwargs)
            ended = get_writer().now_us()
            timing_ended = _thread_timing_snapshot(at_end=True) if capture_thread_timing else None
            timing = _thread_timing_delta(timing_started, timing_ended, started, ended)
            _emit_targets(targets, argument_spec, args, kwargs, result, "end", started, ended, timing)
            return result

        return async_wrapped

    @functools.wraps(fn)
    def wrapped(*args: Any, **kwargs: Any) -> Any:
        timing_started = _thread_timing_snapshot() if capture_thread_timing else None
        started = get_writer().now_us()
        _emit_targets(targets, argument_spec, args, kwargs, None, "start", started, started, None)
        result = fn(*args, **kwargs)
        ended = get_writer().now_us()
        timing_ended = _thread_timing_snapshot(at_end=True) if capture_thread_timing else None
        timing = _thread_timing_delta(timing_started, timing_ended, started, ended)
        _emit_targets(targets, argument_spec, args, kwargs, result, "end", started, ended, timing)
        return result

    return wrapped


def _emit_targets(
    targets: tuple[TargetSpec, ...],
    signature: inspect.Signature | None,
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
    phase: str,
    start_us: int,
    end_us: int,
    thread_timing: dict[str, Any] | None,
) -> None:
    """对同一个 callable 上绑定的多个 target 逐一发事件。"""

    arguments = None
    for target in targets:
        if phase not in target.events:
            continue
        binding_start = _emission_clock() if target.capture_emission_timing else None
        if arguments is None:
            arguments = _bind_arguments(signature, args, kwargs)
        # Mutable request snapshots and fact metadata belong to one event.
        bound = dict(arguments) if len(targets) > 1 else arguments
        _emit(target, bound, args, result, phase, start_us, end_us, thread_timing, binding_start)


def _emit(
    target: TargetSpec,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    result: Any,
    phase: str,
    start_us: int,
    end_us: int,
    thread_timing: dict[str, Any] | None,
    binding_start: tuple[int, int] | None,
) -> None:
    """构造 Chrome trace event。"""

    bound["__trace_sim_phase"] = phase
    bound["__trace_sim_fact_consumers"] = target.fact.consumers
    if not _should_emit_target(target, bound, args, result):
        return
    fields_start = _emission_clock() if target.capture_emission_timing else None
    fields, missing = _collect_fields(target, bound, args, result)
    fields_end = _emission_clock() if target.capture_emission_timing else None
    event_name = target.events[phase]
    base_args = {
        "domain": "python_probe",
        "target_id": target.id,
        "target": target.target,
        "phase": phase,
        "status": "completed" if phase == "end" else phase,
        "missing_required_fields": missing,
    }
    if target.capture_thread_timing and thread_timing is not None:
        base_args.update(thread_timing)
    writer = get_writer()
    write_start = _emission_clock() if target.capture_emission_timing else None
    writer.duration_event(
        event_name,
        start_us,
        end_us,
        "python_probe",
        {
            **base_args,
            "event_kind": event_name,
            **fields,
            "fact": {
                "class": target.fact.fact_class,
                "role": target.fact.role,
                "consumers": list(target.fact.consumers),
            },
        },
    )
    if target.capture_emission_timing:
        write_end = _emission_clock()
        # These diagnostic writes are deliberately outside the measured stages.
        for stage, begin, finish in (
            ("binding", binding_start, fields_start),
            ("fields", fields_start, fields_end),
            ("writer", write_start, write_end),
        ):
            writer.duration_event(
                "python_probe.emission_work",
                begin[0],
                finish[0],
                "runtime_diagnostic",
                {
                    "observed_target_id": target.id,
                    "phase": phase,
                    "stage": stage,
                    "observed_start_us": start_us,
                    "observed_end_us": end_us,
                    "thread_cpu_ns": finish[1] - begin[1],
                },
            )


def _emission_clock() -> tuple[int, int]:
    return time.time_ns() // 1000, time.thread_time_ns()


def _collect_fields(
    target: TargetSpec,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    result: Any,
) -> tuple[dict[str, Any], list[str]]:
    """采集 target 字段。"""

    fields: dict[str, Any] = {}
    missing: list[str] = []
    for field in target.fields:
        found, value = _extract_field(field, bound, args, result)
        if found:
            fields[field.name] = value
        elif field.required:
            missing.append(field.name)
    return fields, missing


def _should_emit_target(
    target: TargetSpec,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    result: Any,
) -> bool:
    """判断 target 的所有 emit_when 条件是否满足。"""

    for source in target.emit_when:
        found, value = extract_value(source, "_emit_when", bound, args, result)
        if not found or not _has_value(value):
            return False
    return True


def _has_value(value: Any) -> bool:
    """判断条件表达式中的值是否算作存在。"""

    if value is None:
        return False
    if isinstance(value, str):
        return bool(value)
    if isinstance(value, (list, tuple, set, dict)):
        return len(value) > 0
    return True


def _bind_arguments(
    signature: inspect.Signature | None, args: tuple[Any, ...], kwargs: dict[str, Any]
) -> dict[str, Any]:
    """Resolve positional, keyword, and default arguments into one field context."""

    values = {f"arg{index}": value for index, value in enumerate(args)}
    values.update(kwargs)
    if signature is None:
        return values

    try:
        binding = signature.bind_partial(*args, **kwargs)
        binding.apply_defaults()
        values.update(binding.arguments)
    except (TypeError, ValueError):
        pass
    return values


def _extract_field(
    field: FieldSpec,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    result: Any,
) -> tuple[bool, Any]:
    """提取单个字段；writer 统一负责最终 JSON 收敛。"""

    source = field.source.strip()
    try:
        found, value = extract_value(source, field.name, bound, args, result)
        return (found, value if found else None)
    except Exception as exc:
        return (False, {"extract_error": type(exc).__name__})


TARGET_MODULES = tuple(sorted(_load_targets()))
