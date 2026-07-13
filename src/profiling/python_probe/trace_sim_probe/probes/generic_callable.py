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
from dataclasses import dataclass
from types import ModuleType
from typing import Any, Callable

from trace_sim_probe.patching import PATCH_MARKER
from trace_sim_probe.schema import validate_hicache_fact
from trace_sim_probe.writer import get_writer, probe_debug_enabled


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
    op: str = "present"
    value: Any = None


@dataclass(frozen=True)
class TargetSpec:
    """一个可被通用 callable probe 包装的目标函数配置。"""

    id: str
    module_name: str
    qualname: str
    target: str
    events: dict[str, str]
    fields: tuple[FieldSpec, ...]
    fact: FactSpec
    emit_when: tuple[EmitCondition, ...] = ()


_TARGETS = None
_PATCHED: set[str] = set()
_FULL_LIST_KEYS = {
    "token_ids",
    "hash_value",
}
SourceExtractor = Callable[
    [str, str, dict[str, Any], tuple[Any, ...], dict[str, Any], Any],
    tuple[bool, bool, Any],
]
_SOURCE_EXTRACTORS: list[SourceExtractor] = []


def register_source_extractor(extractor: SourceExtractor) -> None:
    """注册非通用 source 解析器。

    `generic_callable` 只内置 Python 通用取值语法。面向具体子模块的 source，
    例如 HiCache page hash，应由对应 probe 插件注册。
    """

    _SOURCE_EXTRACTORS.append(extractor)


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


def configured_consumers() -> set[str]:
    """返回当前 target 配置实际声明的 consumer 集合。"""

    return {consumer for target in _load_targets() for consumer in target.fact.consumers}


def install(module: ModuleType) -> None:
    """在模块加载后安装该模块匹配的 callable wrapper。"""

    targets_by_qualname: dict[str, list[TargetSpec]] = {}
    for target in _load_targets():
        if module.__name__ == target.module_name:
            targets_by_qualname.setdefault(target.qualname, []).append(target)

    for targets in targets_by_qualname.values():
        target = targets[0]
        patch_key = f"{target.module_name}:{target.qualname}"
        if patch_key in _PATCHED:
            continue
        resolved = _resolve_target(module, target)
        if resolved is None:
            continue
        owner, attr_name, original = resolved
        if original is None or getattr(original, PATCH_MARKER, False) or not callable(original):
            continue
        wrapped = _wrap_callable(tuple(targets), original)
        setattr(wrapped, PATCH_MARKER, True)
        setattr(owner, attr_name, wrapped)
        _PATCHED.add(patch_key)
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
    return TargetSpec(
        id=target_id,
        module_name=module_name,
        qualname=target,
        target=target,
        events=events,
        fields=tuple(fields),
        fact=fact,
        emit_when=emit_when,
    )


def _resolve_target(module: ModuleType, target: TargetSpec) -> tuple[Any, str, Any] | None:
    """解析模块内 callable。

    返回 owner 对象、owner 上的属性名和原始 callable。owner 可能是 module，也可能
    是类对象；这样同一个 generic probe 可以覆盖模块函数和类方法。
    """

    parts = [part for part in target.qualname.split(".") if part]
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

    allowed = {"start", "end", "exception", "instant"}
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
    op = str(raw.get("op") or "present")
    allowed = {"present", "exists", "has", "absent", "missing", "truthy", "falsey", "equals", "eq", "not_equals", "ne"}
    if op not in allowed:
        raise ValueError(f"python_probe target {target_id!r} emit_when[{index}].op is unsupported: {op!r}")
    return EmitCondition(source=source, op=op, value=raw.get("value"))


def _wrap_callable(targets: tuple[TargetSpec, ...], fn: Callable[..., Any]) -> Callable[..., Any]:
    """包装同步或异步 callable，并按 target phase 配置发事件。"""

    if inspect.iscoroutinefunction(fn):

        @functools.wraps(fn)
        async def async_wrapped(*args: Any, **kwargs: Any) -> Any:
            started = get_writer().now_us()
            _emit_call_phase(targets, fn, args, kwargs, None, "start", started, started)
            try:
                result = await fn(*args, **kwargs)
            except BaseException:
                ended = get_writer().now_us()
                _emit_call_phase(targets, fn, args, kwargs, None, "exception", started, ended)
                raise
            ended = get_writer().now_us()
            _emit_call_phase(targets, fn, args, kwargs, result, "end", started, ended)
            _emit_call_phase(targets, fn, args, kwargs, result, "instant", ended, ended)
            return result

        return async_wrapped

    @functools.wraps(fn)
    def wrapped(*args: Any, **kwargs: Any) -> Any:
        started = get_writer().now_us()
        _emit_call_phase(targets, fn, args, kwargs, None, "start", started, started)
        try:
            result = fn(*args, **kwargs)
        except BaseException:
            ended = get_writer().now_us()
            _emit_call_phase(targets, fn, args, kwargs, None, "exception", started, ended)
            raise
        ended = get_writer().now_us()
        _emit_call_phase(targets, fn, args, kwargs, result, "end", started, ended)
        _emit_call_phase(targets, fn, args, kwargs, result, "instant", ended, ended)
        return result

    return wrapped


def _emit_call_phase(
    targets: tuple[TargetSpec, ...],
    fn: Callable[..., Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
    phase: str,
    start_us: int,
    end_us: int,
) -> None:
    """发射某次 callable 调用的一个阶段事件。"""

    _emit_targets(targets, fn, args, kwargs, result, phase, start_us, end_us)


def _emit_targets(
    targets: tuple[TargetSpec, ...],
    fn: Callable[..., Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
    phase: str,
    start_us: int,
    end_us: int,
) -> None:
    """对同一个 callable 上绑定的多个 target 逐一发事件。"""

    for target in targets:
        if phase not in target.events:
            continue
        _emit(target, fn, args, kwargs, result, phase, start_us, end_us)


def _emit(
    target: TargetSpec,
    fn: Callable[..., Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
    phase: str,
    start_us: int,
    end_us: int,
) -> None:
    """构造 Chrome trace event。"""

    bound = _bind_arguments(fn, args, kwargs)
    _bind_trace_context(bound, target, phase)
    if not _should_emit_target(target, bound, args, kwargs, result):
        return
    fields, missing = _collect_fields(target, bound, args, kwargs, result)
    event_name = _event_name(target, phase)
    base_args = {
        "schema_version": 1,
        "domain": "python_probe",
        "target_id": target.id,
        "target": target.target,
        "phase": phase,
        "status": "completed" if phase in {"end", "instant"} else phase,
        "missing_required_fields": missing,
    }
    get_writer().duration_event(
        event_name,
        start_us,
        end_us,
        "python_probe",
        {
            **base_args,
            "event_kind": event_name,
            **fields,
            **_fact_args(target),
        },
    )


def _bind_trace_context(bound: dict[str, Any], target: TargetSpec, phase: str) -> None:
    """把 fact 元数据注入 source extractor 可见的取值上下文。"""

    bound["__trace_sim_phase"] = phase
    bound["__trace_sim_fact_class"] = target.fact.fact_class
    bound["__trace_sim_fact_role"] = target.fact.role
    bound["__trace_sim_fact_consumers"] = target.fact.consumers


def _event_name(target: TargetSpec, phase: str) -> str:
    """按 target 配置和 phase 生成事件名。"""

    return target.events[phase]


def _collect_fields(
    target: TargetSpec,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[dict[str, Any], list[str]]:
    """采集 target 字段。"""

    fields: dict[str, Any] = {}
    missing: list[str] = []
    for field in target.fields:
        found, value = _extract_field(field, bound, args, kwargs, result)
        if found:
            fields[field.name] = value
        elif field.required:
            missing.append(field.name)
    return fields, missing


def _fact_args(target: TargetSpec) -> dict[str, Any]:
    """生成写入事件 args 的 fact 元数据。"""

    fact = target.fact
    return {
        "fact": {
            "class": fact.fact_class,
            "role": fact.role,
            "consumers": list(fact.consumers),
        }
    }


def _should_emit_target(
    target: TargetSpec,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> bool:
    """判断 target 的所有 emit_when 条件是否满足。"""

    for condition in target.emit_when:
        if not _condition_matches(condition, bound, args, kwargs, result):
            return False
    return True


def _condition_matches(
    condition: EmitCondition,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> bool:
    """执行单个 emit_when 条件判断。"""

    found, value = _extract_raw_value(condition.source, "_emit_when", bound, args, kwargs, result)
    op = condition.op
    if op in ("present", "exists", "has"):
        return found and _has_value(value)
    if op in ("absent", "missing"):
        return not found or not _has_value(value)
    if op == "truthy":
        return found and bool(value)
    if op == "falsey":
        return (not found) or not bool(value)
    if op in ("equals", "eq"):
        return found and value == condition.value
    if op in ("not_equals", "ne"):
        return (not found) or value != condition.value
    return False


def _has_value(value: Any) -> bool:
    """判断条件表达式中的值是否算作存在。"""

    if value is None:
        return False
    if isinstance(value, str):
        return bool(value)
    if isinstance(value, (list, tuple, set, dict)):
        return len(value) > 0
    return True


def _bind_arguments(fn: Callable[..., Any], args: tuple[Any, ...], kwargs: dict[str, Any]) -> dict[str, Any]:
    """Resolve positional, keyword, and default arguments into one field context."""

    values = {f"arg{index}": value for index, value in enumerate(args)}
    values.update(kwargs)
    try:
        binding = inspect.signature(fn).bind_partial(*args, **kwargs)
        binding.apply_defaults()
        values.update(binding.arguments)
    except (TypeError, ValueError):
        pass
    return values


def _extract_field(
    field: FieldSpec,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """提取单个字段，并把普通对象收敛成 JSON 可写形态。"""

    source = field.source.strip()
    try:
        found, value = _extract_raw_value(source, field.name, bound, args, kwargs, result)
        return (found, _jsonable(value, key=field.name) if found else None)
    except Exception as exc:
        return (False, {"extract_error": type(exc).__name__})
    return (False, None)


def _extract_raw_value(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """按 source 表达式读取原始值。

    这里返回原始对象，外层再做 JSON 摘要。这样 `len:<source>` 可以先拿到
    tensor/list/tuple 等对象，再记录长度，避免把大对象字符串化后才取长度。
    """

    handled, found, value = _extract_transform_source(source, field_name, bound, args, kwargs, result)
    if handled:
        return (found, value)

    handled, found, value = _extract_custom_source(source, field_name, bound, args, kwargs, result)
    if handled:
        return (found, value)

    return _extract_builtin_source(source, field_name, bound, args, kwargs, result)


def _extract_transform_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 `len:` / `list:` 这类包装型 source。"""

    if source.startswith("len:"):
        found, value = _extract_raw_value(source.split(":", 1)[1], field_name, bound, args, kwargs, result)
        if not found:
            return (True, False, None)
        return (True, True, _safe_len(value))
    if source.startswith("list:"):
        found, value = _extract_raw_value(source.split(":", 1)[1], field_name, bound, args, kwargs, result)
        if not found:
            return (True, False, None)
        return (True, True, _safe_list(value))
    return (False, False, None)


def _extract_custom_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """把专用 source 交给已注册的插件解析器。"""

    for extractor in _SOURCE_EXTRACTORS:
        handled, found, value = extractor(source, field_name, bound, args, kwargs, result)
        if handled:
            return (True, found, value)
    return (False, False, None)


def _extract_builtin_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """解析 generic callable probe 内置的 source 语法。"""

    if not source:
        return (field_name in bound, bound.get(field_name))
    if source.startswith("arg:"):
        return _extract_arg_source(source.split(":", 1)[1], bound, args)
    if source.startswith("args."):
        return _extract_args_source(source.split(".", 1)[1], args)
    if source.startswith("kwarg:"):
        return _extract_kwarg_source(source.split(":", 1)[1], kwargs)
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

    head, path = _split_head_path(key)
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


def _extract_args_source(key: str, args: tuple[Any, ...]) -> tuple[bool, Any]:
    """读取 `args.<index>[.<path>]`。"""

    head, path = _split_head_path(key)
    if not head.isdigit():
        return (False, None)
    index = int(head)
    if index >= len(args):
        return (False, None)
    value = args[index]
    return _read_path(value, path) if path else (True, value)


def _extract_kwarg_source(key: str, kwargs: dict[str, Any]) -> tuple[bool, Any]:
    """读取 `kwarg:<name>[.<path>]`。"""

    head, path = _split_head_path(key)
    if head not in kwargs:
        return (False, None)
    value = kwargs[head]
    return _read_path(value, path) if path else (True, value)


def _split_head_path(value: str) -> tuple[str, str]:
    """把 `head.tail.path` 拆成首段和剩余路径。"""

    head, separator, path = value.partition(".")
    if not separator:
        return (head, "")
    return (head, path)


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
                if not hasattr(cursor, part):
                    return (False, None)
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


def _jsonable(value: Any, *, key: str | None = None) -> Any:
    """把任意 Python 对象收敛成 JSON 值，保留建模所需 token/path 列表。"""

    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, (list, tuple)):
        items = value if key in _FULL_LIST_KEYS else value[:32]
        return [_jsonable(item) for item in items]
    if isinstance(value, dict):
        return {str(child_key): _jsonable(item, key=str(child_key)) for child_key, item in list(value.items())[:64]}
    return str(value)


TARGET_MODULES = tuple(sorted({target.module_name for target in _load_targets()}))
