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
from trace_sim_probe.writer import get_writer, probe_debug_enabled


@dataclass(frozen=True)
class FieldSpec:
    name: str
    source: str = ""
    required: bool = True


@dataclass(frozen=True)
class TargetSpec:
    id: str
    module_name: str
    qualname: str
    target: str
    events: tuple[str, ...]
    fields: tuple[FieldSpec, ...]
    enabled: bool = True


_TARGETS = None
_PATCHED: set[str] = set()
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
    global _TARGETS
    if _TARGETS is not None:
        return _TARGETS
    raw = os.environ.get("TRACE_SIM_PYTHON_PROBE_TARGETS", "[]")
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        data = []
    targets: list[TargetSpec] = []
    if isinstance(data, list):
        for item in data:
            if isinstance(item, dict):
                target = _parse_target(item)
                if target is not None and target.enabled:
                    targets.append(target)
    _TARGETS = targets
    return targets


def install(module: ModuleType) -> None:
    for target in _load_targets():
        if module.__name__ != target.module_name or target.id in _PATCHED:
            continue
        resolved = _resolve_target(module, target)
        if resolved is None:
            continue
        owner, attr_name, original = resolved
        if original is None or getattr(original, PATCH_MARKER, False) or not callable(original):
            continue
        wrapped = _wrap_callable(target, original)
        setattr(wrapped, PATCH_MARKER, True)
        setattr(owner, attr_name, wrapped)
        _PATCHED.add(target.id)
        if probe_debug_enabled():
            print(f"[trace_sim_probe] patched {target.target}", file=sys.stderr)


def _parse_target(raw: dict[str, Any]) -> TargetSpec | None:
    target_id = raw.get("id")
    target = raw.get("target")
    if not isinstance(target_id, str) or not isinstance(target, str):
        return None
    module_name, qualname = _split_target(raw, target)
    if not module_name or not qualname:
        return None
    fields = []
    for item in raw.get("fields", []):
        field = _parse_field(item)
        if field is not None:
            fields.append(field)
    events_raw = raw.get("events", [])
    events = tuple(item for item in events_raw if isinstance(item, str))
    return TargetSpec(
        id=target_id,
        module_name=module_name,
        qualname=qualname,
        target=target,
        events=events,
        fields=tuple(fields),
        enabled=bool(raw.get("enabled", True)),
    )


def _split_target(raw: dict[str, Any], target: str) -> tuple[str, str]:
    """把配置目标拆成“导入模块”和“模块内对象路径”。

    Python probe 只负责 Python 侧插桩。为了避免 runner 维护一套复杂目标解析，
    配置可以显式写 `module`；若未写，则保留旧版使用习惯：模块级函数用完整路径，
    形如 `pkg.mod.fn`。对常见类方法路径，若倒数第二段像类名，则按
    `pkg.mod.Class.method` 拆成 `pkg.mod` + `Class.method`。
    """

    explicit_module = raw.get("module")
    if isinstance(explicit_module, str) and explicit_module:
        return explicit_module, target

    parts = [part for part in target.split(".") if part]
    if len(parts) < 2:
        return "", ""
    if len(parts) >= 3 and parts[-2][:1].isupper():
        return ".".join(parts[:-2]), ".".join(parts[-2:])
    return ".".join(parts[:-1]), parts[-1]


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


def _parse_field(raw: Any) -> FieldSpec | None:
    if isinstance(raw, str) and raw:
        return FieldSpec(name=raw)
    if isinstance(raw, dict) and isinstance(raw.get("name"), str):
        return FieldSpec(
            name=raw["name"],
            source=str(raw.get("source") or ""),
            required=bool(raw.get("required", True)),
        )
    return None


def _wrap_callable(target: TargetSpec, fn: Callable[..., Any]) -> Callable[..., Any]:
    if inspect.iscoroutinefunction(fn):
        @functools.wraps(fn)
        async def async_wrapped(*args: Any, **kwargs: Any) -> Any:
            started = get_writer().now_us()
            _emit(target, fn, args, kwargs, None, "start", started, started)
            try:
                result = await fn(*args, **kwargs)
            except BaseException:
                ended = get_writer().now_us()
                _emit(target, fn, args, kwargs, None, "exception", started, ended)
                raise
            ended = get_writer().now_us()
            _emit(target, fn, args, kwargs, result, "end", started, ended)
            return result

        return async_wrapped

    @functools.wraps(fn)
    def wrapped(*args: Any, **kwargs: Any) -> Any:
        started = get_writer().now_us()
        _emit(target, fn, args, kwargs, None, "start", started, started)
        try:
            result = fn(*args, **kwargs)
        except BaseException:
            ended = get_writer().now_us()
            _emit(target, fn, args, kwargs, None, "exception", started, ended)
            raise
        ended = get_writer().now_us()
        _emit(target, fn, args, kwargs, result, "end", started, ended)
        return result

    return wrapped


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
    fields, missing = _collect_fields(target, fn, args, kwargs, result)
    event_name = _event_name(target, phase)
    get_writer().duration_event(
        event_name,
        start_us,
        end_us,
        "python_probe",
        {
            "schema_version": 1,
            "model_input": True,
            "domain": "python_probe",
            "event_kind": event_name,
            "target_id": target.id,
            "target": target.target,
            "phase": phase,
            "status": "completed" if phase == "end" else phase,
            "missing_required_fields": missing,
            **fields,
        },
    )


def _event_name(target: TargetSpec, phase: str) -> str:
    if not target.events:
        return f"{target.id}:{phase}"
    if phase == "start":
        return target.events[0]
    if phase == "end":
        return target.events[-1]
    return f"{target.events[-1]}:exception"


def _collect_fields(
    target: TargetSpec,
    fn: Callable[..., Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[dict[str, Any], list[str]]:
    bound = _bind_arguments(fn, args, kwargs)
    fields: dict[str, Any] = {}
    missing: list[str] = []
    for field in target.fields:
        found, value = _extract_field(field, bound, args, kwargs, result)
        if found:
            fields[field.name] = value
        elif field.required:
            missing.append(field.name)
    return fields, missing


def _bind_arguments(fn: Callable[..., Any], args: tuple[Any, ...], kwargs: dict[str, Any]) -> dict[str, Any]:
    values = {f"arg{index}": value for index, value in enumerate(args)}
    values.update(kwargs)
    try:
        binding = inspect.signature(fn).bind_partial(*args, **kwargs)
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
    source = field.source.strip()
    try:
        found, value = _extract_raw_value(source, field.name, bound, args, kwargs, result)
        return (found, _jsonable(value) if found else None)
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

    if source.startswith("len:"):
        found, value = _extract_raw_value(source.split(":", 1)[1], field_name, bound, args, kwargs, result)
        if not found:
            return (False, None)
        return (True, _safe_len(value))
    if source.startswith("list:"):
        found, value = _extract_raw_value(source.split(":", 1)[1], field_name, bound, args, kwargs, result)
        if not found:
            return (False, None)
        return (True, _safe_list(value))
    for extractor in _SOURCE_EXTRACTORS:
        handled, found, value = extractor(source, field_name, bound, args, kwargs, result)
        if handled:
            return (found, value)

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
        return (True, _read_path(args[0], source.split(".", 1)[1]))
    if source == "return":
        return (result is not None, result)
    if source.startswith("return."):
        if result is None:
            return (False, None)
        return (True, _read_path(result, source.split(".", 1)[1]))
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
    return (True, _read_path(value, path) if path else value)


def _extract_args_source(key: str, args: tuple[Any, ...]) -> tuple[bool, Any]:
    """读取 `args.<index>[.<path>]`。"""

    head, path = _split_head_path(key)
    if not head.isdigit():
        return (False, None)
    index = int(head)
    if index >= len(args):
        return (False, None)
    value = args[index]
    return (True, _read_path(value, path) if path else value)


def _extract_kwarg_source(key: str, kwargs: dict[str, Any]) -> tuple[bool, Any]:
    """读取 `kwarg:<name>[.<path>]`。"""

    head, path = _split_head_path(key)
    if head not in kwargs:
        return (False, None)
    value = kwargs[head]
    return (True, _read_path(value, path) if path else value)


def _split_head_path(value: str) -> tuple[str, str]:
    head, separator, path = value.partition(".")
    if not separator:
        return (head, "")
    return (head, path)


def _read_path(obj: Any, path: str) -> Any:
    """按点分路径读取属性、dict key 或 list/tuple 下标。"""

    cursor = obj
    for part in path.split("."):
        if not part:
            continue
        if isinstance(cursor, dict):
            cursor = cursor[part]
        elif isinstance(cursor, (list, tuple)) and part.isdigit():
            cursor = cursor[int(part)]
        else:
            cursor = getattr(cursor, part)
    return cursor


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


def _jsonable(value: Any) -> Any:
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, (list, tuple)):
        return [_jsonable(item) for item in value[:32]]
    if isinstance(value, dict):
        return {str(key): _jsonable(item) for key, item in list(value.items())[:64]}
    return str(value)


TARGET_MODULES = tuple(sorted({target.module_name for target in _load_targets()}))
