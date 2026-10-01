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
from dataclasses import dataclass
from types import ModuleType
from typing import Any, Callable

from trace_sim_probe.patching import PATCH_MARKER
from trace_sim_probe.schema import validate_hicache_fact
from trace_sim_probe.writer import get_writer, probe_debug_enabled
from .thread_timing import (
    _thread_timing_delta,
    _thread_timing_snapshot,
)


@dataclass(frozen=True)
class FieldSpec:
    """单个 probe 字段的配置描述。"""

    name: str
    source: str = ""
    required: bool = True


@dataclass(frozen=True)
class FactSpec:
    """target 级 fact 元数据。"""

    fact_class: str
    role: str
    consumers: tuple[str, ...]


@dataclass(frozen=True)
class EmitCondition:
    """控制某个 target 是否在当前调用阶段发事件的条件。"""

    source: str


@dataclass(frozen=True)
class TargetSpec:
    """一个可被通用 callable probe 包装的目标函数配置。"""

    id: str
    module_name: str
    target: str
    events: dict[str, str]
    fields: tuple[FieldSpec, ...]
    fact: FactSpec
    emit_when: tuple[EmitCondition, ...] = ()
    capture_thread_timing: bool = False
    capture_emission_timing: bool = False


_TARGETS = None
SourceExtractor = Callable[
    [str, str, dict[str, Any], tuple[Any, ...], Any],
    tuple[bool, bool, Any],
]


@functools.cache
def _source_extractors() -> tuple[SourceExtractor, ...]:
    # HiCache helpers use the generic value reader. Defer this import until
    # wrapper installation to avoid a cycle, before any measured calls run.
    from .hicache.sources import HICACHE_SOURCE_EXTRACTORS

    return HICACHE_SOURCE_EXTRACTORS


def _load_targets() -> list[TargetSpec]:
    """从环境变量加载并缓存 Python probe target 配置。"""

    global _TARGETS
    if _TARGETS is not None:
        return _TARGETS
    raw = os.environ.get("TRACE_SIM_PYTHON_PROBE_TARGETS", "[]")
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ValueError("TRACE_SIM_PYTHON_PROBE_TARGETS must be a JSON array") from exc
    targets: list[TargetSpec] = []
    if not isinstance(data, list):
        raise ValueError("TRACE_SIM_PYTHON_PROBE_TARGETS must be a JSON array")
    for index, item in enumerate(data):
        if not isinstance(item, dict):
            raise ValueError(f"TRACE_SIM_PYTHON_PROBE_TARGETS[{index}] must be an object")
        target = _parse_target(item)
        targets.append(target)
    _TARGETS = targets
    return targets


def install(module: ModuleType) -> None:
    """在模块加载后安装该模块匹配的 callable wrapper。"""

    targets_by_callable: dict[str, list[TargetSpec]] = {}
    for target in _load_targets():
        if module.__name__ == target.module_name:
            targets_by_callable.setdefault(target.target, []).append(target)

    for targets in targets_by_callable.values():
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


def _parse_target(raw: dict[str, Any]) -> TargetSpec:
    """校验并解析单个 target 配置。"""

    target_id = raw.get("id")
    target = raw.get("target")
    module_name = raw.get("module")
    if not isinstance(target_id, str) or not target_id:
        raise ValueError("python_probe target id must be a non-empty string")
    if not isinstance(target, str) or not target:
        raise ValueError(f"python_probe target {target_id!r} target must be a non-empty string")
    if not isinstance(module_name, str) or not module_name:
        raise ValueError(f"python_probe target {target_id!r} module must be a non-empty string")
    raw_fields = raw.get("fields")
    if not isinstance(raw_fields, list):
        raise ValueError(f"python_probe target {target_id!r} fields must be an array")
    fields = [_parse_field(item, target_id, index) for index, item in enumerate(raw_fields)]
    events = _parse_events(raw.get("events"), target_id)
    emit_when = _parse_emit_conditions(raw.get("emit_when"), target_id)
    fact = _parse_fact(raw.get("fact"), target_id)
    capture_thread_timing = raw.get("capture_thread_timing", False)
    if not isinstance(capture_thread_timing, bool):
        raise ValueError(f"python_probe target {target_id!r} capture_thread_timing must be a boolean")
    capture_emission_timing = raw.get("capture_emission_timing", False)
    if not isinstance(capture_emission_timing, bool):
        raise ValueError(f"python_probe target {target_id!r} capture_emission_timing must be a boolean")
    return TargetSpec(
        id=target_id,
        module_name=module_name,
        target=target,
        events=events,
        fields=tuple(fields),
        fact=fact,
        emit_when=emit_when,
        capture_thread_timing=capture_thread_timing,
        capture_emission_timing=capture_emission_timing,
    )


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


def _parse_field(raw: Any, target_id: str, index: int) -> FieldSpec:
    """解析字段配置；字段是当前 catalog 合同的一部分，格式错误直接失败。"""

    if not isinstance(raw, dict):
        raise ValueError(f"python_probe target {target_id!r} fields[{index}] must be an object")
    name = raw.get("name")
    if not isinstance(name, str) or not name:
        raise ValueError(f"python_probe target {target_id!r} fields[{index}].name must be a non-empty string")
    source = raw.get("source", "")
    if source is not None and not isinstance(source, str):
        raise ValueError(f"python_probe target {target_id!r} fields[{index}].source must be a string")
    required = raw.get("required", True)
    if not isinstance(required, bool):
        raise ValueError(f"python_probe target {target_id!r} fields[{index}].required must be a boolean")
    return FieldSpec(name=name, source=source or "", required=required)


def _parse_fact(raw: Any, target_id: str) -> FactSpec:
    """解析 fact 合同，强制 target 显式声明分类和输入边界。"""

    if not isinstance(raw, dict):
        raise ValueError(f"python_probe target {target_id!r} must define fact")
    fact_class = raw.get("class")
    role = raw.get("role")
    consumers = raw.get("consumers")
    if not isinstance(fact_class, str) or not fact_class:
        raise ValueError(f"python_probe target {target_id!r} fact.class must be a non-empty string")
    if not isinstance(role, str) or not role:
        raise ValueError(f"python_probe target {target_id!r} fact.role must be a non-empty string")
    if not isinstance(consumers, list) or not all(isinstance(item, str) and item for item in consumers):
        raise ValueError(f"python_probe target {target_id!r} fact.consumers must be a non-empty string array")
    validate_hicache_fact(fact_class, role, consumers)
    return FactSpec(fact_class=fact_class, role=role, consumers=tuple(consumers))


def _parse_events(raw: Any, target_id: str) -> dict[str, str]:
    """解析 phase 到 trace event name 的显式映射。"""

    allowed = {"start", "end"}
    if not isinstance(raw, dict):
        raise ValueError(f"python_probe target {target_id!r} events must be a phase-to-event-name object")
    events: dict[str, str] = {}
    for phase, event_name in raw.items():
        if phase not in allowed:
            raise ValueError(f"python_probe target {target_id!r} events contains unsupported phase {phase!r}")
        if not isinstance(event_name, str) or not event_name:
            raise ValueError(
                f"python_probe target {target_id!r} event name for phase {phase!r} must be a non-empty string"
            )
        events[phase] = event_name
    if not events:
        raise ValueError(f"python_probe target {target_id!r} events must not be empty")
    return events


def _parse_emit_conditions(raw: Any, target_id: str) -> tuple[EmitCondition, ...]:
    """解析 target 的条件发射规则。"""

    if raw is None:
        return ()
    if not isinstance(raw, list):
        raise ValueError(f"python_probe target {target_id!r} emit_when must be an array")
    return tuple(_parse_emit_condition(item, target_id, index) for index, item in enumerate(raw))


def _parse_emit_condition(raw: Any, target_id: str, index: int) -> EmitCondition:
    """解析单条条件发射规则。"""

    if not isinstance(raw, dict):
        raise ValueError(f"python_probe target {target_id!r} emit_when[{index}] must be an object")
    source = raw.get("source")
    if not isinstance(source, str) or not source:
        raise ValueError(f"python_probe target {target_id!r} emit_when[{index}].source must be a non-empty string")
    op = raw.get("op", "present")
    if op != "present":
        raise ValueError(f"python_probe target {target_id!r} emit_when[{index}].op must be 'present'")
    return EmitCondition(source=source)


def _wrap_callable(targets: tuple[TargetSpec, ...], fn: Callable[..., Any]) -> Callable[..., Any]:
    """包装同步或异步 callable，并按 target phase 配置发事件。"""

    _source_extractors()
    # The installed callable is fixed; only its argument values change per call.
    try:
        argument_spec = inspect.signature(fn)
    except (TypeError, ValueError):
        argument_spec = None
    capture_thread_timing = any(target.capture_thread_timing for target in targets)

    if inspect.iscoroutinefunction(fn):

        @functools.wraps(fn)
        async def async_wrapped(*args: Any, **kwargs: Any) -> Any:
            timing_started = _thread_timing_snapshot(boundary="start") if capture_thread_timing else None
            started = get_writer().now_us()
            _emit_targets(targets, argument_spec, args, kwargs, None, "start", started, started, None)
            result = await fn(*args, **kwargs)
            ended = get_writer().now_us()
            timing_ended = _thread_timing_snapshot(boundary="end") if capture_thread_timing else None
            timing = _thread_timing_delta(timing_started, timing_ended, started, ended)
            _emit_targets(targets, argument_spec, args, kwargs, result, "end", started, ended, timing)
            return result

        return async_wrapped

    @functools.wraps(fn)
    def wrapped(*args: Any, **kwargs: Any) -> Any:
        timing_started = _thread_timing_snapshot(boundary="start") if capture_thread_timing else None
        started = get_writer().now_us()
        _emit_targets(targets, argument_spec, args, kwargs, None, "start", started, started, None)
        result = fn(*args, **kwargs)
        ended = get_writer().now_us()
        timing_ended = _thread_timing_snapshot(boundary="end") if capture_thread_timing else None
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

    for condition in target.emit_when:
        found, value = _extract_raw_value(condition.source, "_emit_when", bound, args, result)
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
        found, value = _extract_raw_value(source, field.name, bound, args, result)
        return (found, value if found else None)
    except Exception as exc:
        return (False, {"extract_error": type(exc).__name__})


def _extract_raw_value(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    result: Any,
) -> tuple[bool, Any]:
    """按 source 表达式读取原始值。

    这里返回原始对象，由writer统一做JSON摘要。这样`len:<source>`可以先拿到
    tensor/list/tuple等对象，再记录长度，避免把大对象字符串化后才取长度。
    """

    handled, found, value = _extract_transform_source(source, field_name, bound, args, result)
    if handled:
        return (found, value)

    for extractor in _source_extractors():
        handled, found, value = extractor(source, field_name, bound, args, result)
        if handled:
            return (found, value)

    return _extract_builtin_source(source, field_name, bound, args, result)


def _extract_transform_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 `len:` / `list:` 这类包装型 source。"""

    if source.startswith("len:"):
        found, value = _extract_raw_value(source.split(":", 1)[1], field_name, bound, args, result)
        if not found:
            return (True, False, None)
        return (True, True, _safe_len(value))
    if source.startswith("list:"):
        found, value = _extract_raw_value(source.split(":", 1)[1], field_name, bound, args, result)
        if not found:
            return (True, False, None)
        return (True, True, _safe_list(value))
    return (False, False, None)


def _extract_builtin_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    result: Any,
) -> tuple[bool, Any]:
    """解析 generic callable probe 内置的 source 语法。"""

    if not source:
        return (field_name in bound, bound.get(field_name))
    if source.startswith("arg:"):
        return _extract_arg_source(source.split(":", 1)[1], bound, args)
    if source == "self":
        return (bool(args), args[0] if args else None)
    if source.startswith("self."):
        if not args:
            return (False, None)
        return _read_path(args[0], source.split(".", 1)[1])
    if source == "return":
        return (result is not None, result)
    if source.startswith("return."):
        if result is None:
            return (False, None)
        return _read_path(result, source.split(".", 1)[1])
    if source.startswith("const:"):
        return (True, source.split(":", 1)[1])
    return (False, None)


def _extract_arg_source(key: str, bound: dict[str, Any], args: tuple[Any, ...]) -> tuple[bool, Any]:
    """读取 `arg:<name>[.<path>]` 或 `arg:<index>[.<path>]`。"""

    head, _, path = key.partition(".")
    if head.isdigit():
        index = int(head)
        if index >= len(args):
            return (False, None)
        value = args[index]
    else:
        if head not in bound:
            return (False, None)
        value = bound[head]
    return _read_path(value, path) if path else (True, value)


def _read_path(obj: Any, path: str) -> tuple[bool, Any]:
    """按点分路径读取属性、dict key 或 list/tuple 下标。"""

    cursor = obj
    for part in path.split("."):
        if not part:
            continue
        if cursor is None:
            return (False, None)
        try:
            if isinstance(cursor, dict):
                if part not in cursor:
                    return (False, None)
                cursor = cursor[part]
            elif isinstance(cursor, (list, tuple)) and part.isdigit():
                index = int(part)
                if index >= len(cursor):
                    return (False, None)
                cursor = cursor[index]
            else:
                cursor = getattr(cursor, part)
        except (KeyError, IndexError, TypeError, AttributeError):
            return (False, None)
    return (True, cursor)


def _safe_len(value: Any) -> int | None:
    """读取容器长度；无法读取时返回 None，让字段仍然可见但不伪造数值。"""

    if value is None:
        return None
    try:
        return len(value)  # type: ignore[arg-type]
    except TypeError:
        return None


def _safe_list(value: Any) -> list[Any] | None:
    """把 tensor/array/RadixKey 等容器收敛成短列表。"""

    if value is None:
        return None
    if hasattr(value, "tolist") and callable(value.tolist):
        value = value.tolist()
    elif hasattr(value, "token_ids"):
        value = getattr(value, "token_ids")
    try:
        return list(value)[:64]
    except TypeError:
        return None


TARGET_MODULES = tuple(sorted({target.module_name for target in _load_targets()}))
