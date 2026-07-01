"""HiCache probe source extractor 注册。"""

from __future__ import annotations

from typing import Any

from trace_sim_probe.probes import generic_callable as _base

from .common import _cache_scope_key, _extract_source_value
from .context import _HICACHE_SEQUENCE_BY_SCOPE
from .snapshots import _snapshot_hicache_object
from .tokens import (
    _extract_request_list,
    _extract_request_token_count_records,
    _extract_request_token_path,
    _extract_request_token_path_records,
    _extract_request_token_span,
    _extract_request_token_span_records,
    _extract_request_tokens,
    _extract_token_path,
    _extract_token_span,
    _request_id,
)


def _source_spec(source: str, prefix: str) -> str | None:
    """如果 source 命中特定前缀，返回冒号后的参数段。"""

    if not source.startswith(prefix):
        return None
    return source.split(":", 1)[1]


def _hicache_state_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 `hicache_state:self`，生成 state snapshot 字段。"""

    if source != "hicache_state:self":
        return (False, False, None)
    if not args:
        return (True, False, None)
    snapshot = _snapshot_hicache_object(args[0])
    return (True, True, snapshot)


def _token_path_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 `token_path:` source，生成完整 token dictionary 引用。"""

    spec = _source_spec(source, "token_path:")
    if spec is None:
        return (False, False, None)
    found, value = _extract_token_path(spec, bound, args, kwargs, result)
    return (True, found, value if found else None)


def _token_span_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 `token_span:` source，生成同一 token path 内的 span 引用。"""

    spec = _source_spec(source, "token_span:")
    if spec is None:
        return (False, False, None)
    found, value = _extract_token_span(spec, bound, args, kwargs, result)
    return (True, found, value)


def _request_token_path_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 request 级 token path source，覆盖 active/fill/committed 等模式。"""

    spec = _source_spec(source, "request_token_path:")
    if spec is None:
        return (False, False, None)
    found, value = _extract_request_token_path(spec, bound, args, kwargs, result)
    return (True, found, value if found else None)


def _request_token_span_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 request 级 token span source，避免重复携带完整 token 列表。"""

    spec = _source_spec(source, "request_token_span:")
    if spec is None:
        return (False, False, None)
    found, value = _extract_request_token_span(spec, bound, args, kwargs, result)
    return (True, found, value)


def _request_token_count_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 request token 数量 source，用于 target-derived page 投影。"""

    spec = _source_spec(source, "request_token_count:")
    if spec is None:
        return (False, False, None)
    found, tokens = _extract_request_tokens(spec, bound, args, kwargs, result)
    return (True, found, len(tokens) if found else None)


def _request_ids_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 batch 级 `request_ids:` source。"""

    spec = _source_spec(source, "request_ids:")
    if spec is None:
        return (False, False, None)
    found, requests = _extract_request_list(spec, bound, args, kwargs, result)
    if not found:
        return (True, False, None)
    return (True, True, [_request_id(req) for req in requests])


def _request_positions_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 batch 级 `request_positions:` source。"""

    spec = _source_spec(source, "request_positions:")
    if spec is None:
        return (False, False, None)
    found, requests = _extract_request_list(spec, bound, args, kwargs, result)
    if not found:
        return (True, False, None)
    return (
        True,
        True,
        [
            {
                "request_id": _request_id(req),
                "index": index,
            }
            for index, req in enumerate(requests)
        ],
    )


def _request_token_paths_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 batch 级 request token dictionary 数组。"""

    spec = _source_spec(source, "request_token_paths:")
    if spec is None:
        return (False, False, None)
    found, rows = _extract_request_token_path_records(spec, bound, args, kwargs, result)
    return (True, found, rows if found else None)


def _request_token_spans_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 batch 级 request token span 数组。"""

    spec = _source_spec(source, "request_token_spans:")
    if spec is None:
        return (False, False, None)
    found, rows = _extract_request_token_span_records(spec, bound, args, kwargs, result)
    return (True, found, rows if found else None)


def _request_token_counts_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 batch 级 request token count 数组。"""

    spec = _source_spec(source, "request_token_counts:")
    if spec is None:
        return (False, False, None)
    found, rows = _extract_request_token_count_records(spec, bound, args, kwargs, result)
    return (True, found, rows if found else None)


def _hicache_cache_scope_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """生成 rank/object 绑定的 cache_scope 路由键。"""

    spec = _source_spec(source, "hicache_cache_scope:")
    if spec is None:
        return (False, False, None)
    found, value = _extract_source_value(spec, field_name, bound, args, kwargs, result)
    if not found:
        return (True, False, None)
    return (True, True, _cache_scope_key(value))


def _hicache_seq_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """按 cache_scope 生成单调 seq_no，维持 atomic fact 顺序。"""

    spec = _source_spec(source, "hicache_seq:")
    if spec is None:
        return (False, False, None)
    found, value = _extract_source_value(spec, field_name, bound, args, kwargs, result)
    if not found:
        return (True, False, None)
    scope = _cache_scope_key(value)
    next_seq = _HICACHE_SEQUENCE_BY_SCOPE.get(scope, 0) + 1
    _HICACHE_SEQUENCE_BY_SCOPE[scope] = next_seq
    return (True, True, next_seq)


_HICACHE_SOURCE_EXTRACTORS = (
    _hicache_state_source,
    _token_path_source,
    _token_span_source,
    _request_token_path_source,
    _request_token_span_source,
    _request_token_count_source,
    _request_ids_source,
    _request_positions_source,
    _request_token_paths_source,
    _request_token_spans_source,
    _request_token_counts_source,
    _hicache_cache_scope_source,
    _hicache_seq_source,
)


def register_source_extractors() -> None:
    """注册 HiCache 专用 source extractor。"""

    for extractor in _HICACHE_SOURCE_EXTRACTORS:
        _base.register_source_extractor(extractor)
