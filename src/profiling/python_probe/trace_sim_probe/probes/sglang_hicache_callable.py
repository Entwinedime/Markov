"""SGLang HiCache callable probe。

该 probe 复用 `generic_callable` 的函数包装逻辑，只补充 HiCache 建模需要的
token/range source 和 validation-only state snapshot。这样通用 probe 不包含
HiCache 特化规则，HiCache 新后端也不再依赖按 page size 预声明的 page identity。
"""

from __future__ import annotations

import hashlib
import os
import functools
from types import ModuleType
from typing import Any

from trace_sim_probe.patching import PATCH_MARKER
from trace_sim_probe.probes import generic_callable as _base
from trace_sim_probe.writer import get_writer, probe_debug_enabled


_TOKEN_PATHS_EMITTED_BY_SCOPE: dict[str, set[str]] = {}
_HICACHE_SEQUENCE_BY_SCOPE: dict[str, int] = {}
_TOKEN_HASH_ALGO = "sglang_radix_sha256_v1"
_INTERNAL_PATCHED: set[str] = set()
_INTERNAL_TARGET_MODULES = (
    "sglang.srt.mem_cache.events",
    "sglang.srt.mem_cache.hiradix_cache",
    "sglang.srt.mem_cache.radix_cache",
    "sglang.srt.managers.cache_controller",
)


def _truthy(value: str | None) -> bool:
    """解析 probe 环境变量中常见的 true/false 写法。"""

    return value is not None and value.lower() not in ("", "0", "false", "no", "off")


def _hicache_state_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 `hicache_state:self`，只生成 validation-only state snapshot。"""

    if source != "hicache_state:self":
        return (False, False, None)
    if not _truthy(os.environ.get("TRACE_SIM_HICACHE_STATE_TRACE")):
        return (
            True,
            True,
            _base.ExtractedField(
                {"enabled": False},
                model_input=False,
                event_kind="state_snapshot",
                extra_args={
                    "dag_input": False,
                    "fact_class": "oracle_state",
                },
            ),
        )
    if not args:
        return (True, False, None)
    snapshot = _snapshot_hicache_object(args[0])
    return (
        True,
        True,
        _base.ExtractedField(
            snapshot,
            model_input=False,
            event_kind="state_snapshot",
            extra_args={
                "dag_input": False,
                "fact_class": "oracle_state",
            },
        ),
    )


def _token_path_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 `token_path:` source，生成完整 token dictionary 引用。"""

    if not source.startswith("token_path:"):
        return (False, False, None)
    found, value = _extract_token_path(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, _base.ExtractedField(value) if found else None)


def _token_span_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 `token_span:` source，生成同一 token path 内的 span 引用。"""

    if not source.startswith("token_span:"):
        return (False, False, None)
    found, value = _extract_token_span(source.split(":", 1)[1], bound, args, kwargs, result)
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

    if not source.startswith("request_token_path:"):
        return (False, False, None)
    found, value = _extract_request_token_path(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, _base.ExtractedField(value) if found else None)


def _request_token_span_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 request 级 token span source，避免重复携带完整 token 列表。"""

    if not source.startswith("request_token_span:"):
        return (False, False, None)
    found, value = _extract_request_token_span(source.split(":", 1)[1], bound, args, kwargs, result)
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

    if not source.startswith("request_token_count:"):
        return (False, False, None)
    found, tokens = _extract_request_tokens(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, len(tokens) if found else None)


def _token_path_concat_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 prefix/suffix 拼接后的 token path source。"""

    if not source.startswith("token_path_concat:"):
        return (False, False, None)
    found, value = _extract_token_path_concat(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, _base.ExtractedField(value) if found else None)


def _token_span_concat_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """处理 prefix/suffix 拼接后的 token span source。"""

    if not source.startswith("token_span_concat:"):
        return (False, False, None)
    found, value = _extract_token_span_concat(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, value)


def _node_token_path_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """从 radix node 反推出根到当前节点的完整 token path。"""

    if not source.startswith("node_token_path:"):
        return (False, False, None)
    found, value = _extract_node_token_path(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, _base.ExtractedField(value) if found else None)


def _node_token_span_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """从 radix node 生成完整路径的 span 描述。"""

    if not source.startswith("node_token_span:"):
        return (False, False, None)
    found, value = _extract_node_token_span(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, value)


def _node_token_count_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """读取 radix node 的完整路径 token 数量。"""

    if not source.startswith("node_token_count:"):
        return (False, False, None)
    found, node = _extract_source_value(source.split(":", 1)[1], field_name, bound, args, kwargs, result)
    if not found:
        return (True, False, None)
    return (True, True, len(_full_key_tokens(node)))


def _hicache_node_summary_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """采集单个 HiCache node 的调试摘要。"""

    if not source.startswith("hicache_node_summary:"):
        return (False, False, None)
    found, node = _extract_source_value(source.split(":", 1)[1], field_name, bound, args, kwargs, result)
    if not found or node is None:
        return (True, False, None)
    return (True, True, _node_summary_record(node))


def _hicache_node_chain_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """采集 node 到 root 的链路摘要，辅助定位 radix tree 结构变化。"""

    if not source.startswith("hicache_node_chain:"):
        return (False, False, None)
    found, node = _extract_source_value(source.split(":", 1)[1], field_name, bound, args, kwargs, result)
    if not found or node is None:
        return (True, False, None)
    return (True, True, _node_chain_record(node))


def _hicache_evictable_snapshot_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """采集 evictable device/host leaf 集合摘要。"""

    if not source.startswith("hicache_evictable_snapshot:"):
        return (False, False, None)
    found, cache = _extract_source_value(source.split(":", 1)[1], field_name, bound, args, kwargs, result)
    if not found or cache is None:
        return (True, False, None)
    return (True, True, _evictable_snapshot_record(cache))


def _hicache_prefetch_progress_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """采集某个 request 的 prefetch 进度证据。"""

    if not source.startswith("hicache_prefetch_progress:"):
        return (False, False, None)
    found, value = _extract_prefetch_progress(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, value)


def _hicache_request_runtime_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """采集 request 对象上的运行时长度和 anchor 字段。"""

    if not source.startswith("hicache_request_runtime:"):
        return (False, False, None)
    found, req = _extract_source_value(source.split(":", 1)[1], field_name, bound, args, kwargs, result)
    if not found or req is None:
        return (True, False, None)
    return (True, True, _request_runtime_record(req))


def _hicache_scheduler_prefetch_state_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """采集 scheduler 视角的 prefetch 判定上下文。"""

    if not source.startswith("hicache_scheduler_prefetch_state:"):
        return (False, False, None)
    parts = [part.strip() for part in source.split(":", 1)[1].split(",") if part.strip()]
    if len(parts) < 2:
        return (True, False, None)
    found_scheduler, scheduler = _extract_source_value(parts[0], "scheduler", bound, args, kwargs, result)
    found_req, req = _extract_source_value(parts[1], "request", bound, args, kwargs, result)
    if not found_scheduler or scheduler is None or not found_req or req is None:
        return (True, False, None)
    return (True, True, _scheduler_prefetch_state_record(scheduler, req))


def _node_token_path_concat_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """把 node 路径和 suffix token 拼成新的 token dictionary。"""

    if not source.startswith("node_token_path_concat:"):
        return (False, False, None)
    found, value = _extract_node_token_path_concat(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, _base.ExtractedField(value) if found else None)


def _node_token_span_concat_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """把 node 路径和 suffix token 拼成 span 描述。"""

    if not source.startswith("node_token_span_concat:"):
        return (False, False, None)
    found, value = _extract_node_token_span_concat(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, value)


def _hicache_cache_scope_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """生成 rank/object 绑定的 cache_scope 路由键。"""

    if not source.startswith("hicache_cache_scope:"):
        return (False, False, None)
    found, value = _extract_source_value(source.split(":", 1)[1], field_name, bound, args, kwargs, result)
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

    if not source.startswith("hicache_seq:"):
        return (False, False, None)
    found, value = _extract_source_value(source.split(":", 1)[1], field_name, bound, args, kwargs, result)
    if not found:
        return (True, False, None)
    scope = _cache_scope_key(value)
    next_seq = _HICACHE_SEQUENCE_BY_SCOPE.get(scope, 0) + 1
    _HICACHE_SEQUENCE_BY_SCOPE[scope] = next_seq
    return (True, True, next_seq)


def _hicache_config_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """从 HiCache 对象抽取 capacity/policy 配置事实。"""

    if not source.startswith("hicache_config:"):
        return (False, False, None)
    parts = [part.strip() for part in source.split(":", 1)[1].split(",") if part.strip()]
    if not parts:
        return (True, False, None)
    found, value = _extract_source_value(parts[0], field_name, bound, args, kwargs, result)
    if not found:
        return (True, False, None)
    config = _cache_config_record(value)
    if len(parts) == 1:
        return (True, True, config)
    selected = config.get(parts[1])
    return (True, selected is not None, selected)


def _hicache_requested_pages_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    """按 source page size 把请求 token 数投影成页数。"""

    if not source.startswith("hicache_requested_pages:"):
        return (False, False, None)
    parts = [part.strip() for part in source.split(":", 1)[1].split(",") if part.strip()]
    if len(parts) < 2:
        return (True, False, None)
    found_tokens, token_value = _extract_source_value(parts[0], field_name, bound, args, kwargs, result)
    found_cache, cache_value = _extract_source_value(parts[1], "cache_scope", bound, args, kwargs, result)
    requested_tokens = _safe_int(token_value) if found_tokens else None
    page_size = _safe_int(getattr(cache_value, "page_size", None)) if found_cache else None
    if requested_tokens is None:
        return (True, False, None)
    return (
        True,
        True,
        {
            "requested_tokens": requested_tokens,
            "source_page_size": page_size,
            "requested_pages": _ceil_div(requested_tokens, page_size),
        },
    )


def _extract_token_path(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """按 source spec 读取 token path 并生成 dictionary 记录。"""

    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if not parts:
        return (False, None)
    found, value = _extract_source_value(parts[0], "token_path", bound, args, kwargs, result)
    if not found:
        return (False, None)
    scope = _scope_from_optional_source(parts[1], bound, args, kwargs, result) if len(parts) > 1 else ""
    return (True, _token_path_record(_tokens_for_path(value), scope))


def _extract_token_span(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """按 source spec 读取完整 token path，并返回覆盖全路径的 span。"""

    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if not parts:
        return (False, None)
    found, value = _extract_source_value(parts[0], "token_span", bound, args, kwargs, result)
    if not found:
        return (False, None)
    tokens = _tokens_for_path(value)
    return (True, _token_span_record(tokens, 0, len(tokens)))


def _extract_request_token_path(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """从 request 对象提取指定模式的 token path。"""

    found, tokens, scope = _extract_request_tokens_and_scope(spec, bound, args, kwargs, result)
    if not found:
        return (False, None)
    return (True, _token_path_record(tokens, scope))


def _extract_request_token_span(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """从 request 对象提取指定模式的 token span。"""

    found, tokens, _scope = _extract_request_tokens_and_scope(spec, bound, args, kwargs, result)
    if not found:
        return (False, None)
    return (True, _token_span_record(tokens, 0, len(tokens)))


def _extract_request_tokens(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, list[Any]]:
    """从 request source spec 返回 token 列表。"""

    found, tokens, _scope = _extract_request_tokens_and_scope(spec, bound, args, kwargs, result)
    return (found, tokens)


def _extract_request_tokens_and_scope(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, list[Any], str]:
    """解析 request source spec，返回 token 列表和可选 cache scope。"""

    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if not parts:
        return (False, [], "")
    found_req, req = _extract_source_value(parts[0], "request", bound, args, kwargs, result)
    if not found_req or req is None:
        return (False, [], "")
    mode = parts[1] if len(parts) > 1 else "active"
    scope = _scope_from_optional_source(parts[2], bound, args, kwargs, result) if len(parts) > 2 else ""
    tokens = _request_tokens(req, mode)
    return (True, tokens, scope)


def _extract_token_path_concat(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """拼接两个 token source 并生成 token dictionary。"""

    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if len(parts) < 2:
        return (False, None)
    found_prefix, prefix = _extract_source_value(parts[0], "prefix_tokens", bound, args, kwargs, result)
    found_suffix, suffix = _extract_source_value(parts[1], "suffix_tokens", bound, args, kwargs, result)
    if not found_prefix or not found_suffix:
        return (False, None)
    scope = _scope_from_optional_source(parts[2], bound, args, kwargs, result) if len(parts) > 2 else ""
    tokens = _tokens_for_path(prefix) + _tokens_for_path(suffix)
    return (True, _token_path_record(tokens, scope))


def _extract_token_span_concat(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """拼接两个 token source 并生成覆盖全路径的 span。"""

    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if len(parts) < 2:
        return (False, None)
    found_prefix, prefix = _extract_source_value(parts[0], "prefix_tokens", bound, args, kwargs, result)
    found_suffix, suffix = _extract_source_value(parts[1], "suffix_tokens", bound, args, kwargs, result)
    if not found_prefix or not found_suffix:
        return (False, None)
    tokens = _tokens_for_path(prefix) + _tokens_for_path(suffix)
    return (True, _token_span_record(tokens, 0, len(tokens)))


def _extract_node_token_path(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """从 radix node 提取根到节点的 token path dictionary。"""

    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if not parts:
        return (False, None)
    found, node = _extract_source_value(parts[0], "node_token_path", bound, args, kwargs, result)
    if not found or node is None:
        return (False, None)
    scope = _scope_from_optional_source(parts[1], bound, args, kwargs, result) if len(parts) > 1 else ""
    return (True, _token_path_record(_full_key_tokens(node), scope))


def _extract_node_token_span(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """从 radix node 提取根到节点的 token span。"""

    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if not parts:
        return (False, None)
    found, node = _extract_source_value(parts[0], "node_token_span", bound, args, kwargs, result)
    if not found or node is None:
        return (False, None)
    tokens = _full_key_tokens(node)
    return (True, _token_span_record(tokens, 0, len(tokens)))


def _extract_node_token_path_concat(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """把 radix node 全路径和 suffix token 合成为 dictionary。"""

    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if len(parts) < 2:
        return (False, None)
    found_node, node = _extract_source_value(parts[0], "prefix_node", bound, args, kwargs, result)
    found_suffix, suffix = _extract_source_value(parts[1], "suffix_tokens", bound, args, kwargs, result)
    if not found_node or node is None or not found_suffix:
        return (False, None)
    scope = _scope_from_optional_source(parts[2], bound, args, kwargs, result) if len(parts) > 2 else ""
    tokens = _full_key_tokens(node) + _tokens_for_path(suffix)
    return (True, _token_path_record(tokens, scope))


def _extract_node_token_span_concat(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """把 radix node 全路径和 suffix token 合成为 span。"""

    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if len(parts) < 2:
        return (False, None)
    found_node, node = _extract_source_value(parts[0], "prefix_node", bound, args, kwargs, result)
    found_suffix, suffix = _extract_source_value(parts[1], "suffix_tokens", bound, args, kwargs, result)
    if not found_node or node is None or not found_suffix:
        return (False, None)
    tokens = _full_key_tokens(node) + _tokens_for_path(suffix)
    return (True, _token_span_record(tokens, 0, len(tokens)))


def _extract_source_value(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """委托通用 probe source 语法读取原始值。"""

    return _base._extract_raw_value(source, field_name, bound, args, kwargs, result)


def _scope_from_optional_source(
    source: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> str:
    """从可选 source 读取 cache_scope，缺失时返回空字符串。"""

    found, value = _extract_source_value(source, "cache_scope", bound, args, kwargs, result)
    return _cache_scope_key(value) if found else ""


def _cache_scope_key(value: Any) -> str:
    """生成包含 rank 和对象身份的 cache scope 路由键。"""

    rank = os.environ.get("RANK", os.environ.get("LOCAL_RANK", "unknown"))
    if value is None:
        return f"rank:{rank}:unknown"
    if isinstance(value, (str, int, float, bool)):
        return f"rank:{rank}:{value}"
    return f"rank:{rank}:{type(value).__name__}:{id(value)}"


def _token_span_record(tokens: list[Any], begin: int, end: int) -> dict[str, Any]:
    """生成 token span 记录，引用同一 hash 算法下的 path id。"""

    return {
        "path_id": _token_path_id(tokens),
        "begin": begin,
        "end": end,
        "token_count": len(tokens),
        "hash_algo": _TOKEN_HASH_ALGO,
    }


def _token_path_record(tokens: list[Any], scope: str = "") -> dict[str, Any]:
    """生成 token dictionary 记录，并在同一 scope 内只携带一次 token_ids。"""

    path_id = _token_path_id(tokens)
    row: dict[str, Any] = {
        "token_path_id": path_id,
        "token_count": len(tokens),
        "hash_algo": _TOKEN_HASH_ALGO,
    }
    scope_key = scope or "global"
    emitted = _TOKEN_PATHS_EMITTED_BY_SCOPE.setdefault(scope_key, set())
    if path_id not in emitted:
        emitted.add(path_id)
        row["token_ids"] = _jsonable_token_ids(tokens)
    return row


def _token_path_id(tokens: list[Any]) -> str:
    """按 SGLang token 序列生成稳定 path hash。"""

    hasher = hashlib.sha256()
    for token in tokens:
        _hash_one_token_id(hasher, token)
    return "sha256_u32le:" + hasher.hexdigest()


def _jsonable_token_ids(tokens: list[Any]) -> list[Any]:
    """把 token id 转成 JSON 可写整数列表。"""

    result: list[Any] = []
    for token in tokens:
        if isinstance(token, (list, tuple)):
            result.append([int(item) for item in token])
        else:
            result.append(int(token))
    return result


def _hash_one_token_id(hasher: "hashlib._Hash", token: Any) -> None:
    """把单个 token 或复合 token 按 u32le 写入 hash。"""

    if isinstance(token, (list, tuple)):
        for item in token:
            hasher.update(int(item).to_bytes(4, byteorder="little", signed=False))
    else:
        hasher.update(int(token).to_bytes(4, byteorder="little", signed=False))


def _safe_int(value: Any) -> int | None:
    """宽松解析整数，避免 None/bool 污染容量和长度字段。"""

    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _tokens_for_path(value: Any) -> list[Any]:
    """读取完整 token 序列。

    通用 probe 的 `_safe_list` 会截断到 64 个元素，适合作摘要；token dictionary
    需要完整 token 序列，不能在这里截断。
    """

    if value is None:
        return []
    if hasattr(value, "tolist") and callable(value.tolist):
        value = value.tolist()
    elif hasattr(value, "token_ids"):
        value = getattr(value, "token_ids")
    try:
        return list(value)
    except TypeError:
        return []


def _request_tokens(req: Any, mode: str) -> list[Any]:
    """按 SGLang request 阶段选择可建模 token 序列。"""

    normalized = (mode or "active").lower()
    if normalized == "fill":
        return _tokens_for_path(getattr(req, "fill_ids", None))
    if normalized in ("origin_output", "full"):
        return _tokens_for_path(getattr(req, "origin_input_ids", None)) + _tokens_for_path(getattr(req, "output_ids", None))
    if normalized in ("committed", "cache_committed"):
        tokens = _tokens_for_path(getattr(req, "origin_input_ids", None)) + _tokens_for_path(getattr(req, "output_ids", None))
        committed = _safe_int(getattr(req, "kv_committed_len", None))
        cache_commit_len = getattr(req, "_cache_commit_len", None)
        if callable(cache_commit_len):
            try:
                committed = _safe_int(cache_commit_len())
            except Exception:
                pass
        if committed is not None:
            return tokens[:committed]
        return tokens
    if normalized == "output":
        return _tokens_for_path(getattr(req, "output_ids", None))
    fill = _tokens_for_path(getattr(req, "fill_ids", None))
    if fill:
        return fill
    return _tokens_for_path(getattr(req, "origin_input_ids", None)) + _tokens_for_path(getattr(req, "output_ids", None))


def _cache_config_record(obj: Any) -> dict[str, Any]:
    """生成 HiCache 配置事实摘要，供 target config 与质量审计使用。"""

    capacity = _snapshot_capacity(obj)
    thresholds = {
        "write_through_threshold": capacity.get("write_through_threshold"),
        "load_back_threshold": capacity.get("load_back_threshold"),
        "prefetch_threshold_tokens": capacity.get("prefetch_threshold_tokens"),
        "prefetch_threshold_pages": capacity.get("prefetch_threshold_pages"),
        "prefetch_capacity_limit_tokens": capacity.get("prefetch_capacity_limit_tokens"),
        "prefetch_capacity_limit_pages": capacity.get("prefetch_capacity_limit_pages"),
        "storage_batch_size": capacity.get("storage_batch_size"),
    }
    capacity_summary = {
        "l1_capacity_tokens": capacity.get("l1_capacity_tokens"),
        "l1_capacity_pages": capacity.get("l1_capacity_pages"),
        "l2_capacity_tokens": capacity.get("l2_capacity_tokens"),
        "l2_capacity_pages": capacity.get("l2_capacity_pages"),
        "enable_storage": capacity.get("enable_storage"),
    }
    policy_params = {
        "write_policy": capacity.get("write_policy"),
        "prefetch_policy": capacity.get("prefetch_policy"),
        "thresholds": thresholds,
    }
    return {
        "source_page_size": capacity.get("page_size"),
        "write_policy": capacity.get("write_policy"),
        "prefetch_policy": capacity.get("prefetch_policy"),
        "thresholds": thresholds,
        "capacity_summary": capacity_summary,
        "dynamic_capacity_observed": {
            "l1_available_tokens": capacity.get("l1_available_tokens"),
            "l1_available_pages": capacity.get("l1_available_pages"),
            "l2_available_tokens": capacity.get("l2_available_tokens"),
            "l2_available_pages": capacity.get("l2_available_pages"),
            "prefetch_tokens_occupied": capacity.get("prefetch_tokens_occupied"),
        },
        "policy_params": policy_params,
    }


def _snapshot_hicache_object(obj: Any) -> dict[str, Any]:
    """读取 SGLang HiCache 对象的轻量状态快照。

    该函数只在显式验证模式下执行。实现使用宽松 introspection，是为了同时适配
    HiRadixCache、HiCacheController 和最小替身对象；缺失字段会记录为空，
    不影响业务执行。
    """

    nodes = _collect_radix_nodes(obj)
    derived = _derive_page_sets(nodes)
    return {
        "enabled": True,
        "object_type": type(obj).__name__,
        "object_id": f"{type(obj).__name__}:{id(obj)}",
        "page_size": _safe_int(getattr(obj, "page_size", None)),
        "nodes": nodes,
        "derived": derived,
        "capacity": _snapshot_capacity(obj),
        "controller": _snapshot_controller(obj),
    }


def _collect_radix_nodes(obj: Any) -> list[dict[str, Any]]:
    """遍历可能的 radix tree 入口，采集去重后的 node 快照。"""

    candidates = []
    for attr in ("root_node", "root", "tree_root", "node"):
        if hasattr(obj, attr):
            candidates.append(getattr(obj, attr))
    for attr in ("nodes", "node_map"):
        value = getattr(obj, attr, None)
        if isinstance(value, dict):
            candidates.extend(value.values())
        elif isinstance(value, (list, tuple, set)):
            candidates.extend(value)

    result: list[dict[str, Any]] = []
    seen: set[int] = set()
    stack = [item for item in candidates if item is not None]
    while stack and len(result) < 4096:
        node = stack.pop()
        identity = id(node)
        if identity in seen:
            continue
        seen.add(identity)
        row = _snapshot_node(node)
        result.append(row)
        stack.extend(_iter_children(node))
    return result


def _iter_children(node: Any) -> list[Any]:
    """读取 radix node 的 children 集合，兼容不同字段名。"""

    children = []
    for attr in ("children", "childs", "child_nodes"):
        value = getattr(node, attr, None)
        if isinstance(value, dict):
            children.extend(value.values())
        elif isinstance(value, (list, tuple, set)):
            children.extend(value)
    return [item for item in children if item is not None]


def _snapshot_node(node: Any) -> dict[str, Any]:
    """采集单个 radix node 的状态字段。"""

    parent = getattr(node, "parent", None)
    hash_value = _jsonable_compact(getattr(node, "hash_value", None))
    key_value = getattr(node, "key", getattr(node, "token_ids", None))
    device_value = _first_attr(node, ("value", "device_value", "device_indices"))
    host_value = _first_attr(node, ("host_value", "host_indices"))
    row = {
        "node_id": _jsonable_compact(_first_attr(node, ("id", "node_id"))),
        "parent_id": _jsonable_compact(_first_attr(parent, ("id", "node_id")) if parent is not None else None),
        "hash_value": hash_value,
        "key_token_length": _base._safe_len(key_value),
        "has_device_value": device_value is not None,
        "has_host_value": host_value is not None,
        "evicted": bool(getattr(node, "evicted", False)),
        "backuped": bool(getattr(node, "backuped", getattr(node, "backed_up", False))),
        "dirty": bool(getattr(node, "dirty", False)),
        "hit_count": _safe_int(getattr(node, "hit_count", 0)) or 0,
        "lock_ref": _safe_int(_first_attr(node, ("lock_ref", "lock_ref_count", "lock_ref_counter"))) or 0,
        "host_ref_counter": _safe_int(getattr(node, "host_ref_counter", 0)) or 0,
        "child_count": len(_iter_children(node)),
    }
    return row


def _node_summary_record(node: Any) -> dict[str, Any]:
    """生成更适合事件 payload 的 node 摘要。"""

    parent = getattr(node, "parent", None)
    key_value = getattr(node, "key", getattr(node, "token_ids", None))
    value = _first_attr(node, ("value", "device_value", "device_indices"))
    host_value = _first_attr(node, ("host_value", "host_indices"))
    full_tokens = _full_key_tokens(node)
    return {
        "node_id": _jsonable_compact(_first_attr(node, ("id", "node_id"))),
        "parent_id": _jsonable_compact(_first_attr(parent, ("id", "node_id")) if parent is not None else None),
        "key_token_count": _base._safe_len(key_value),
        "full_token_count": len(full_tokens),
        "span": _token_span_record(full_tokens, 0, len(full_tokens)) if full_tokens else None,
        "hash_value": _jsonable_compact(getattr(node, "hash_value", None)),
        "has_device_value": value is not None,
        "device_token_count": _base._safe_len(value),
        "has_host_value": host_value is not None,
        "host_token_count": _base._safe_len(host_value),
        "evicted": bool(getattr(node, "evicted", False)),
        "backuped": bool(getattr(node, "backuped", getattr(node, "backed_up", False))),
        "hit_count": _safe_int(getattr(node, "hit_count", 0)) or 0,
        "lock_ref": _safe_int(_first_attr(node, ("lock_ref", "lock_ref_count", "lock_ref_counter"))) or 0,
        "host_ref_counter": _safe_int(getattr(node, "host_ref_counter", 0)) or 0,
        "child_count": len(_iter_children(node)),
        "priority": _safe_int(getattr(node, "priority", None)),
    }


def _node_chain_record(node: Any) -> list[dict[str, Any]]:
    """采集 root 到当前 node 的摘要链，限制长度避免 trace 膨胀。"""

    chain = []
    seen: set[int] = set()
    current = node
    while current is not None and id(current) not in seen and len(chain) < 128:
        seen.add(id(current))
        chain.append(current)
        current = getattr(current, "parent", None)
    return [_node_summary_record(item) for item in reversed(chain)]


def _evictable_snapshot_record(cache: Any) -> dict[str, Any]:
    """采集 device/host evictable leaf 的数量和样本。"""

    device_leaves = list(getattr(cache, "evictable_leaves", []) or [])
    host_leaves = list(getattr(cache, "evictable_host_leaves", []) or [])
    return {
        "evictable_size_tokens": _safe_int(getattr(cache, "evictable_size_", None)),
        "protected_size_tokens": _safe_int(getattr(cache, "protected_size_", None)),
        "device_leaf_count": len(device_leaves),
        "host_leaf_count": len(host_leaves),
        "device_leaf_sample": [_node_summary_record(node) for node in device_leaves[:32]],
        "host_leaf_sample": [_node_summary_record(node) for node in host_leaves[:32]],
    }


def _full_key_tokens(node: Any, cache: dict[int, list[Any]] | None = None) -> list[Any]:
    """沿 parent 链拼出 radix node 的完整 token key。"""

    if node is None:
        return []
    identity = id(node)
    if cache is not None and identity in cache:
        return cache[identity]
    chain = []
    seen: set[int] = set()
    current = node
    while current is not None and id(current) not in seen:
        seen.add(id(current))
        chain.append(current)
        current = getattr(current, "parent", None)
    tokens: list[Any] = []
    for item in reversed(chain):
        tokens.extend(_tokens_for_path(getattr(item, "key", getattr(item, "token_ids", None))))
    if cache is not None:
        cache[identity] = tokens
    return tokens


def _snapshot_controller(obj: Any) -> dict[str, Any]:
    """采集 HiCache controller 队列和当前 storage operation 摘要。"""

    controller = _first_attr(obj, ("cache_controller", "controller")) or obj
    queues = {}
    for attr in ("load_queue", "write_queue", "prefetch_queue", "backup_queue", "ack_queue"):
        queues[attr] = _base._safe_len(getattr(controller, attr, None)) or 0
    ongoing = _first_attr(controller, ("ongoing_operation", "current_operation", "operation"))
    storage = _first_attr(controller, ("storage", "storage_backend", "file_backend"))
    return {
        "queues": queues,
        "ongoing_operation_id": _jsonable_compact(_first_attr(ongoing, ("id", "operation_id")) if ongoing is not None else None),
        "ongoing_hash_pages": _jsonable_compact(_first_attr(ongoing, ("hash_value", "hash_pages")) if ongoing is not None else None),
        "completed_tokens": _safe_int(_first_attr(ongoing, ("completed_tokens",))) if ongoing is not None else None,
        "prefetch_occupied_tokens": _safe_int(getattr(controller, "prefetch_occupied_tokens", None)),
        "storage_backend_enabled": storage is not None,
        "storage_backend_type": type(storage).__name__ if storage is not None else "",
    }


def _extract_prefetch_progress(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, dict[str, Any]]:
    """读取 request 当前 prefetch 进度和 operation 完成证据。"""

    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if len(parts) < 2:
        return (False, {})
    found_cache, cache = _extract_source_value(parts[0], "cache", bound, args, kwargs, result)
    found_req, req_id = _extract_source_value(parts[1], "request_id", bound, args, kwargs, result)
    if not found_cache or cache is None or not found_req:
        return (False, {})
    page_size = _safe_int(getattr(cache, "page_size", None)) or 0
    ongoing = getattr(cache, "ongoing_prefetch", {}) or {}
    entry = ongoing.get(req_id) if isinstance(ongoing, dict) else None
    loaded_by_req = getattr(cache, "prefetch_loaded_tokens_by_reqid", {}) or {}
    row: dict[str, Any] = {
        "request_id": str(req_id),
        "check_return": result if isinstance(result, bool) else None,
        "has_ongoing_prefetch": entry is not None,
        "loaded_tokens_evidence": _safe_int(loaded_by_req.get(req_id)) if isinstance(loaded_by_req, dict) else None,
        "page_size": page_size or None,
    }
    if entry is None:
        return (True, row)
    try:
        last_host_node, prefetch_key, host_indices, operation = entry
    except Exception:
        return (True, row)
    completed_tokens = _safe_int(getattr(operation, "completed_tokens", None)) or 0
    operation_hash_pages = _jsonable_compact(getattr(operation, "hash_value", None)) or []
    token_count = _base._safe_len(prefetch_key) or 0
    row.update(
        {
            "last_host_node": _node_summary_record(last_host_node),
            "last_host_node_chain": _node_chain_record(last_host_node),
            "prefetch_token_count": token_count,
            "host_token_count": _base._safe_len(host_indices),
            "operation_id": _jsonable_compact(getattr(operation, "id", None)),
            "operation_hash_pages": operation_hash_pages,
            "completed_tokens": completed_tokens,
            "ready_pages_estimate": completed_tokens // page_size if page_size > 0 else None,
            "late_tokens_estimate": max(0, token_count - completed_tokens),
            "operation_terminated": _safe_call_bool(operation, "is_terminated"),
        }
    )
    return (True, row)


def _request_runtime_record(req: Any) -> dict[str, Any]:
    """采集 request 生命周期中与 cache state 相关的运行时字段。"""

    commit_len = _safe_int(getattr(req, "kv_committed_len", None))
    cache_commit_len = getattr(req, "_cache_commit_len", None)
    effective_commit_len = commit_len
    if callable(cache_commit_len):
        try:
            effective_commit_len = _safe_int(cache_commit_len())
        except Exception:
            pass
    return {
        "request_id": _jsonable_compact(getattr(req, "rid", None)),
        "fill_tokens": _base._safe_len(getattr(req, "fill_ids", None)),
        "origin_input_tokens": _base._safe_len(getattr(req, "origin_input_ids", None)),
        "output_tokens": _base._safe_len(getattr(req, "output_ids", None)),
        "kv_committed_len": commit_len,
        "effective_commit_len": effective_commit_len,
        "kv_committed_freed": bool(getattr(req, "kv_committed_freed", False)),
        "cache_protected_len": _safe_int(getattr(req, "cache_protected_len", None)),
        "extend_input_len": _safe_int(getattr(req, "extend_input_len", None)),
        "prefix_tokens": _base._safe_len(getattr(req, "prefix_indices", None)),
        "host_hit_length": _safe_int(getattr(req, "host_hit_length", None)),
        "storage_hit_length": _safe_int(getattr(req, "storage_hit_length", None)),
        "priority": _safe_int(getattr(req, "priority", None)),
        "last_node": _summary_or_none(getattr(req, "last_node", None)),
        "last_node_chain": _chain_or_empty(getattr(req, "last_node", None)),
        "last_host_node": _summary_or_none(getattr(req, "last_host_node", None)),
        "last_host_node_chain": _chain_or_empty(getattr(req, "last_host_node", None)),
        "best_match_node": _summary_or_none(getattr(req, "best_match_node", None)),
    }


def _scheduler_prefetch_state_record(scheduler: Any, req: Any) -> dict[str, Any]:
    """采集 scheduler 做 prefetch 判定时可见的上下文。"""

    tree_cache = getattr(scheduler, "tree_cache", None)
    root_node = getattr(tree_cache, "root_node", None)
    fill_tokens = _request_tokens(req, "fill")
    prefix_tokens = _base._safe_len(getattr(req, "prefix_indices", None)) or 0
    host_hit_length = _safe_int(getattr(req, "host_hit_length", None)) or 0
    matched_len = max(0, prefix_tokens + host_hit_length)
    last_host_node = getattr(req, "last_host_node", None)
    anchor_backuped = bool(getattr(last_host_node, "backuped", False)) if last_host_node is not None else False
    anchor_is_root = last_host_node is not None and root_node is not None and last_host_node is root_node
    new_input_tokens = fill_tokens[matched_len:]
    prefix_keys = None
    if last_host_node is not None and getattr(tree_cache, "hicache_storage_pass_prefix_keys", False):
        get_prefix_hash_values = getattr(last_host_node, "get_prefix_hash_values", None)
        if callable(get_prefix_hash_values):
            try:
                prefix_keys = get_prefix_hash_values(getattr(last_host_node, "parent", None))
            except Exception:
                prefix_keys = None
    last_hash = None
    get_last_hash_value = getattr(last_host_node, "get_last_hash_value", None)
    if callable(get_last_hash_value):
        try:
            last_hash = get_last_hash_value()
        except Exception:
            last_hash = None
    return {
        "request": _request_runtime_record(req),
        "cache_scope": _cache_scope_key(tree_cache),
        "source_page_size": _safe_int(getattr(tree_cache, "page_size", None)),
        "enable_hicache_storage": bool(getattr(scheduler, "enable_hicache_storage", False)),
        "matched_len": matched_len,
        "new_input_tokens": len(new_input_tokens),
        "prefetch_anchor_eligible": anchor_backuped or anchor_is_root,
        "last_host_node_backuped": anchor_backuped,
        "last_host_node_is_root": anchor_is_root,
        "last_hash": _jsonable_compact(last_hash),
        "prefix_keys": _jsonable_compact(prefix_keys),
        "policy_params": _cache_config_record(tree_cache).get("policy_params") if tree_cache is not None else None,
        "queue_snapshot": _async_queue_snapshot(tree_cache) if tree_cache is not None else None,
    }


def _summary_or_none(node: Any) -> dict[str, Any] | None:
    """可空 node 摘要辅助函数。"""

    return _node_summary_record(node) if node is not None else None


def _chain_or_empty(node: Any) -> list[dict[str, Any]]:
    """可空 node chain 辅助函数。"""

    return _node_chain_record(node) if node is not None else []


def _snapshot_capacity(obj: Any) -> dict[str, Any]:
    """采集 HiCache 容量和策略事实。

    capacity 信息只进入 validation-only state snapshot。它用于解释 target
    配置下的有效 L1/L2 budget，不能作为业务 DAG 事件消费。
    """

    controller = _first_attr(obj, ("cache_controller", "controller")) or obj
    page_size = _safe_int(_first_attr(obj, ("page_size",))) or _safe_int(_first_attr(controller, ("page_size",)))
    device_pool = (
        _first_attr(controller, ("mem_pool_device", "device_pool"))
        or _first_attr(obj, ("mem_pool_device", "device_pool", "kv_cache", "token_to_kv_pool_device"))
    )
    host_pool = (
        _first_attr(controller, ("mem_pool_host", "host_pool"))
        or _first_attr(obj, ("mem_pool_host", "host_pool", "token_to_kv_pool_host"))
    )
    l1 = _snapshot_pool_capacity(device_pool, page_size)
    l2 = _snapshot_pool_capacity(host_pool, page_size)
    prefetch_capacity_limit_tokens = _safe_int(_first_attr(controller, ("prefetch_capacity_limit",)))
    prefetch_threshold_tokens = _safe_int(_first_attr(controller, ("prefetch_threshold",)))
    return {
        "page_size": page_size,
        "write_policy": _jsonable_compact(_first_attr(controller, ("write_policy",))),
        "prefetch_policy": _jsonable_compact(_first_attr(obj, ("prefetch_stop_policy", "storage_prefetch_policy"))),
        "write_through_threshold": _safe_int(_first_attr(obj, ("write_through_threshold",))),
        "load_back_threshold": _safe_int(_first_attr(obj, ("load_back_threshold",))),
        "prefetch_threshold_tokens": prefetch_threshold_tokens,
        "prefetch_threshold_pages": _page_count_from_tokens(prefetch_threshold_tokens, page_size),
        "prefetch_capacity_limit_tokens": prefetch_capacity_limit_tokens,
        "prefetch_capacity_limit_pages": _page_count_from_tokens(prefetch_capacity_limit_tokens, page_size),
        "prefetch_tokens_occupied": _safe_int(_first_attr(controller, ("prefetch_tokens_occupied",))),
        "enable_storage": _jsonable_compact(_first_attr(controller, ("enable_storage",))),
        "storage_batch_size": _safe_int(_first_attr(controller, ("storage_batch_size",))),
        "l1_pool": l1,
        "l2_pool": l2,
        "l1_capacity_tokens": l1.get("capacity_tokens"),
        "l1_capacity_pages": l1.get("capacity_pages"),
        "l1_available_tokens": l1.get("available_tokens"),
        "l1_available_pages": l1.get("available_pages"),
        "l2_capacity_tokens": l2.get("capacity_tokens"),
        "l2_capacity_pages": l2.get("capacity_pages"),
        "l2_available_tokens": l2.get("available_tokens"),
        "l2_available_pages": l2.get("available_pages"),
    }


def _snapshot_pool_capacity(pool: Any, page_size: int | None) -> dict[str, Any]:
    """读取 KV pool 容量。

    SGLang host/device pool 的 `size` 通常表示 token slot 数量；按 page_size
    向下取整得到可建模的 page 容量。`page_num` 保留为 raw 字段用于排查
    实现细节，不直接覆盖模型容量。
    """

    capacity_tokens = _safe_int(_first_attr(pool, ("size", "capacity", "num_tokens"))) if pool is not None else None
    available_tokens = _safe_call_int(pool, "available_size") if pool is not None else None
    if available_tokens is None and pool is not None:
        available_tokens = _base._safe_len(_first_attr(pool, ("free_slots", "free_pages")))
    pool_page_size = _safe_int(_first_attr(pool, ("page_size",))) or page_size
    return {
        "pool_type": type(pool).__name__ if pool is not None else "",
        "page_size": pool_page_size,
        "capacity_tokens": capacity_tokens,
        "capacity_pages": _page_count_from_tokens(capacity_tokens, pool_page_size),
        "available_tokens": available_tokens,
        "available_pages": _page_count_from_tokens(available_tokens, pool_page_size),
        "raw_page_num": _safe_int(_first_attr(pool, ("page_num", "num_pages"))) if pool is not None else None,
    }


def _safe_call_int(obj: Any, method_name: str) -> int | None:
    """安全调用无参方法并解析整数结果。"""

    method = getattr(obj, method_name, None) if obj is not None else None
    if not callable(method):
        return None
    try:
        return _safe_int(method())
    except Exception:
        return None


def _safe_call_bool(obj: Any, method_name: str) -> bool | None:
    """安全调用无参方法并解析布尔结果。"""

    method = getattr(obj, method_name, None) if obj is not None else None
    if not callable(method):
        return None
    try:
        return bool(method())
    except Exception:
        return None


def _page_count_from_tokens(tokens: int | None, page_size: int | None) -> int | None:
    """把 token 容量向下投影为完整 page 数。"""

    if tokens is None or page_size is None or page_size <= 0:
        return None
    return tokens // page_size


def _ceil_div(value: int | None, divisor: int | None) -> int | None:
    """按 page size 计算请求页数，保留非整页请求。"""

    if value is None or divisor is None or divisor <= 0:
        return None
    return (value + divisor - 1) // divisor


def _derive_page_sets(nodes: list[dict[str, Any]]) -> dict[str, list[str]]:
    """从 node snapshot 派生 validation-only page state 集合。"""

    l1: set[str] = set()
    l2: set[str] = set()
    dirty: set[str] = set()
    backuped: set[str] = set()
    locked: set[str] = set()
    evicted: set[str] = set()
    for node in nodes:
        pages = _page_keys_from_hash_value(node.get("hash_value"))
        if node.get("has_device_value"):
            l1.update(pages)
        if node.get("has_host_value"):
            l2.update(pages)
        if node.get("dirty"):
            dirty.update(pages)
        if node.get("backuped"):
            backuped.update(pages)
        if node.get("evicted"):
            evicted.update(pages)
        if (node.get("lock_ref") or 0) > 0 or (node.get("host_ref_counter") or 0) > 0:
            locked.update(pages)
    return {
        "l1_resident_pages": sorted(l1),
        "l2_resident_pages": sorted(l2),
        "dirty_pages": sorted(dirty),
        "backuped_pages": sorted(backuped),
        "locked_pages": sorted(locked),
        "evicted_pages": sorted(evicted),
    }


def _page_keys_from_hash_value(value: Any) -> list[str]:
    """把 SGLang hash_value 规整成 page key 字符串列表。"""

    if value is None:
        return []
    if isinstance(value, list):
        return [str(item) for item in value if item is not None]
    return [str(value)]


def _first_attr(obj: Any, names: tuple[str, ...]) -> Any:
    """按候选字段名顺序读取第一个存在的属性。"""

    if obj is None:
        return None
    for name in names:
        if hasattr(obj, name):
            return getattr(obj, name)
    return None


def _jsonable_compact(value: Any) -> Any:
    """把对象收敛为短 JSON 值，控制 trace payload 大小。"""

    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, (list, tuple)):
        return [_jsonable_compact(item) for item in value[:64]]
    if isinstance(value, dict):
        return {str(key): _jsonable_compact(item) for key, item in list(value.items())[:64]}
    return str(value)


def install(module: ModuleType) -> None:
    """安装通用 callable probe，并补充 HiCache 内部 hook。"""

    _base.install(module)
    _install_internal_hicache_hooks(module)


def _install_internal_hicache_hooks(module: ModuleType) -> None:
    """按 SGLang 模块名安装 HiCache source_actual 内部 hook。"""

    module_name = getattr(module, "__name__", "")
    if module_name == "sglang.srt.mem_cache.events":
        mixin = getattr(module, "KVCacheEventMixin", None)
        if mixin is not None:
            _wrap_internal_method(mixin, "_record_store_event", _wrap_record_store_event, f"{module_name}:KVCacheEventMixin._record_store_event")
            _wrap_internal_method(mixin, "_record_remove_event", _wrap_record_remove_event, f"{module_name}:KVCacheEventMixin._record_remove_event")
            _wrap_internal_method(mixin, "_record_all_cleared_event", _wrap_record_all_cleared_event, f"{module_name}:KVCacheEventMixin._record_all_cleared_event")
        return

    if module_name == "sglang.srt.mem_cache.radix_cache":
        radix = getattr(module, "RadixCache", None)
        if radix is not None:
            _wrap_internal_method(radix, "_delete_leaf", _wrap_delete_leaf, f"{module_name}:RadixCache._delete_leaf")
            _wrap_internal_method(radix, "_update_leaf_status", _wrap_update_leaf_status, f"{module_name}:RadixCache._update_leaf_status")
        tree_node = getattr(module, "TreeNode", None)
        if tree_node is not None:
            _wrap_internal_method(tree_node, "protect_host", _wrap_host_ref("protect"), f"{module_name}:TreeNode.protect_host")
            _wrap_internal_method(tree_node, "release_host", _wrap_host_ref("release"), f"{module_name}:TreeNode.release_host")
        return

    if module_name == "sglang.srt.mem_cache.hiradix_cache":
        hiradix = getattr(module, "HiRadixCache", None)
        if hiradix is not None:
            _wrap_internal_method(hiradix, "_split_node", _wrap_split_node, f"{module_name}:HiRadixCache._split_node")
            _wrap_internal_method(hiradix, "_update_host_leaf_status", _wrap_update_host_leaf_status, f"{module_name}:HiRadixCache._update_host_leaf_status")
            _wrap_internal_method(hiradix, "_inc_hit_count", _wrap_hit_count_update, f"{module_name}:HiRadixCache._inc_hit_count")
            _wrap_internal_method(hiradix, "writing_check", _wrap_writing_check, f"{module_name}:HiRadixCache.writing_check")
            _wrap_internal_method(hiradix, "loading_check", _wrap_loading_check, f"{module_name}:HiRadixCache.loading_check")
            _wrap_internal_method(
                hiradix,
                "drain_storage_control_queues",
                _wrap_drain_storage_control_queues,
                f"{module_name}:HiRadixCache.drain_storage_control_queues",
            )
            _wrap_internal_method(hiradix, "init_load_back", _wrap_init_load_back, f"{module_name}:HiRadixCache.init_load_back")
            _wrap_internal_method(hiradix, "load_back", _wrap_load_back, f"{module_name}:HiRadixCache.load_back")
            _wrap_internal_method(hiradix, "write_backup", _wrap_write_backup, f"{module_name}:HiRadixCache.write_backup")
            _wrap_internal_method(hiradix, "write_backup_storage", _wrap_write_backup_storage, f"{module_name}:HiRadixCache.write_backup_storage")
            _wrap_internal_method(hiradix, "evict_host", _wrap_evict_host, f"{module_name}:HiRadixCache.evict_host")
            _wrap_internal_method(hiradix, "terminate_prefetch", _wrap_hiradix_terminate_prefetch, f"{module_name}:HiRadixCache.terminate_prefetch")
            _wrap_internal_method(hiradix, "release_aborted_request", _wrap_release_aborted_request, f"{module_name}:HiRadixCache.release_aborted_request")
            _wrap_internal_method(hiradix, "pop_prefetch_loaded_tokens", _wrap_pop_prefetch_loaded_tokens, f"{module_name}:HiRadixCache.pop_prefetch_loaded_tokens")
        return

    if module_name == "sglang.srt.managers.cache_controller":
        controller = getattr(module, "HiCacheController", None)
        if controller is not None:
            _wrap_internal_method(controller, "load", _wrap_controller_load, f"{module_name}:HiCacheController.load")
            _wrap_internal_method(controller, "start_loading", _wrap_controller_start_loading, f"{module_name}:HiCacheController.start_loading")
            _wrap_internal_method(controller, "write", _wrap_controller_write, f"{module_name}:HiCacheController.write")
            _wrap_internal_method(controller, "start_writing", _wrap_controller_start_writing, f"{module_name}:HiCacheController.start_writing")
            _wrap_internal_method(controller, "prefetch", _wrap_controller_prefetch, f"{module_name}:HiCacheController.prefetch")
            _wrap_internal_method(controller, "prefetch_rate_limited", _wrap_prefetch_rate_limited, f"{module_name}:HiCacheController.prefetch_rate_limited")
            _wrap_internal_method(controller, "_storage_hit_query", _wrap_storage_hit_query, f"{module_name}:HiCacheController._storage_hit_query")
            _wrap_internal_method(controller, "terminate_prefetch", _wrap_terminate_prefetch, f"{module_name}:HiCacheController.terminate_prefetch")
            _wrap_internal_method(
                controller,
                "append_host_mem_release",
                _wrap_append_host_mem_release,
                f"{module_name}:HiCacheController.append_host_mem_release",
            )


def _wrap_internal_method(cls: Any, method_name: str, wrapper_factory: Any, patch_key: str) -> None:
    """给指定类方法安装一次性 wrapper，避免重复插桩。"""

    if patch_key in _INTERNAL_PATCHED:
        return
    method = getattr(cls, method_name, None)
    if method is None or getattr(method, PATCH_MARKER, False):
        return
    wrapped = wrapper_factory(method)
    setattr(wrapped, PATCH_MARKER, True)
    setattr(cls, method_name, wrapped)
    _INTERNAL_PATCHED.add(patch_key)
    if probe_debug_enabled():
        print(f"[trace_sim_probe] patched internal HiCache hook {patch_key}", flush=True)


def _wrap_split_node(method: Any) -> Any:
    """包装 radix split，记录 split 前后 node 结构变化。"""

    @functools.wraps(method)
    def wrapped(self: Any, key: Any, child: Any, split_len: int, *args: Any, **kwargs: Any) -> Any:
        before_child = _node_summary_record(child)
        result = method(self, key, child, split_len, *args, **kwargs)
        _emit_internal_event(
            self,
            "hicache_internal.radix_node_split",
            "radix_node_mutation_observed",
            {
                "mutation_kind": "split",
                "split_len": _safe_int(split_len),
                "before_child": before_child,
                "after_child": _node_summary_record(child),
                "new_parent": _node_summary_record(result),
                "new_parent_chain": _node_chain_record(result),
                "evictable": _evictable_snapshot_record(self),
            },
            target="HiRadixCache._split_node",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_delete_leaf(method: Any) -> Any:
    """包装 radix leaf 删除，记录删除节点及父节点变化。"""

    @functools.wraps(method)
    def wrapped(self: Any, node: Any, *args: Any, **kwargs: Any) -> Any:
        before_node = _node_summary_record(node)
        parent = getattr(node, "parent", None)
        before_parent = _node_summary_record(parent) if parent is not None else None
        result = method(self, node, *args, **kwargs)
        _emit_internal_event(
            self,
            "hicache_internal.radix_node_delete",
            "radix_node_mutation_observed",
            {
                "mutation_kind": "delete_leaf",
                "before_node": before_node,
                "before_parent": before_parent,
                "after_parent": _node_summary_record(parent) if parent is not None else None,
                "evictable": _evictable_snapshot_record(self),
            },
            target="RadixCache._delete_leaf",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_update_leaf_status(method: Any) -> Any:
    """包装 device evictable leaf 状态更新。"""

    @functools.wraps(method)
    def wrapped(self: Any, node: Any, *args: Any, **kwargs: Any) -> Any:
        before_member = _set_contains(getattr(self, "evictable_leaves", None), node)
        before_node = _node_summary_record(node)
        result = method(self, node, *args, **kwargs)
        after_member = _set_contains(getattr(self, "evictable_leaves", None), node)
        if before_member != after_member or probe_debug_enabled():
            _emit_internal_event(
                self,
                "hicache_internal.evictable_device_delta",
                "evictable_state_observed",
                {
                    "tier": "L1",
                    "before_evictable": before_member,
                    "after_evictable": after_member,
                    "before_node": before_node,
                    "after_node": _node_summary_record(node),
                    "evictable": _evictable_snapshot_record(self),
                },
                target="RadixCache._update_leaf_status",
                fact_class="source_actual",
            )
        return result

    return wrapped


def _wrap_update_host_leaf_status(method: Any) -> Any:
    """包装 host evictable leaf 状态更新。"""

    @functools.wraps(method)
    def wrapped(self: Any, node: Any, *args: Any, **kwargs: Any) -> Any:
        before_member = _set_contains(getattr(self, "evictable_host_leaves", None), node)
        before_node = _node_summary_record(node)
        result = method(self, node, *args, **kwargs)
        after_member = _set_contains(getattr(self, "evictable_host_leaves", None), node)
        if before_member != after_member or probe_debug_enabled():
            _emit_internal_event(
                self,
                "hicache_internal.evictable_host_delta",
                "evictable_state_observed",
                {
                    "tier": "L2",
                    "before_evictable": before_member,
                    "after_evictable": after_member,
                    "before_node": before_node,
                    "after_node": _node_summary_record(node),
                    "evictable": _evictable_snapshot_record(self),
                },
                target="HiRadixCache._update_host_leaf_status",
                fact_class="source_actual",
            )
        return result

    return wrapped


def _wrap_record_store_event(method: Any) -> Any:
    """包装 KVCacheEventMixin store 事件，记录 node 存储介质。"""

    @functools.wraps(method)
    def wrapped(self: Any, node: Any, medium: Any = None, *args: Any, **kwargs: Any) -> Any:
        result = method(self, node, medium, *args, **kwargs)
        _emit_internal_event(
            self,
            "hicache_internal.node_store",
            "node_store_observed",
            {
                "medium": _medium_name(medium, default="GPU"),
                "node": _node_summary_record(node),
                "node_chain": _node_chain_record(node),
            },
            target="KVCacheEventMixin._record_store_event",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_record_remove_event(method: Any) -> Any:
    """包装 KVCacheEventMixin remove 事件，记录 node 从介质移除。"""

    @functools.wraps(method)
    def wrapped(self: Any, node: Any, medium: Any = None, *args: Any, **kwargs: Any) -> Any:
        result = method(self, node, medium, *args, **kwargs)
        _emit_internal_event(
            self,
            "hicache_internal.node_remove",
            "node_remove_observed",
            {
                "medium": _medium_name(medium, default="GPU"),
                "node": _node_summary_record(node),
                "node_chain": _node_chain_record(node),
            },
            target="KVCacheEventMixin._record_remove_event",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_record_all_cleared_event(method: Any) -> Any:
    """包装全量清理事件，记录清理后的 evictable 和队列状态。"""

    @functools.wraps(method)
    def wrapped(self: Any, *args: Any, **kwargs: Any) -> Any:
        result = method(self, *args, **kwargs)
        _emit_internal_event(
            self,
            "hicache_internal.all_blocks_cleared",
            "all_blocks_cleared_observed",
            {
                "evictable": _evictable_snapshot_record(self),
                "queue_snapshot": _async_queue_snapshot(self),
            },
            target="KVCacheEventMixin._record_all_cleared_event",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_host_ref(direction: str) -> Any:
    """生成 host ref 计数增减 wrapper factory。"""

    def factory(method: Any) -> Any:
        @functools.wraps(method)
        def wrapped(self: Any, *args: Any, **kwargs: Any) -> Any:
            before = _node_summary_record(self)
            result = method(self, *args, **kwargs)
            after = _node_summary_record(self)
            if before.get("host_ref_counter") != after.get("host_ref_counter") or probe_debug_enabled():
                _emit_internal_event(
                    self,
                    f"hicache_internal.host_ref_{direction}",
                    "host_ref_delta_observed",
                    {
                        "host_ref_direction": direction,
                        "before_node": before,
                        "after_node": after,
                        "node_chain": _node_chain_record(self),
                    },
                    target=f"TreeNode.{direction}_host",
                    fact_class="source_actual",
                )
            return result

        return wrapped

    return factory


def _wrap_hit_count_update(method: Any) -> Any:
    """包装 hit/write 计数更新，记录 write-back 相关 node 标记。"""

    @functools.wraps(method)
    def wrapped(self: Any, node: Any, *args: Any, **kwargs: Any) -> Any:
        before = _node_summary_record(node)
        result = method(self, node, *args, **kwargs)
        after = _node_summary_record(node)
        if before.get("hit_count") != after.get("hit_count") or before.get("backuped") != after.get("backuped") or probe_debug_enabled():
            _emit_internal_event(
                self,
                "hicache_internal.write_counter_delta",
                "write_counter_delta_observed",
                {
                    "before_node": before,
                    "after_node": after,
                    "chunked": bool(args[0]) if args else bool(kwargs.get("chunked", False)),
                    "write_policy": _jsonable_compact(_first_attr(_first_attr(self, ("cache_controller", "controller")) or self, ("write_policy",))),
                    "write_through_threshold": _safe_int(getattr(self, "write_through_threshold", None)),
                },
                target="HiRadixCache._inc_hit_count",
                fact_class="source_actual",
            )
        return result

    return wrapped


def _wrap_init_load_back(method: Any) -> Any:
    """包装 load-back 初始化，记录请求 quota、anchor 和队列变化。"""

    @functools.wraps(method)
    def wrapped(self: Any, params: Any, *args: Any, **kwargs: Any) -> Any:
        before = _async_queue_snapshot(self)
        best_match_node = getattr(params, "best_match_node", None)
        result = method(self, params, *args, **kwargs)
        loaded_values, last_node = _tuple_item(result, 0), _tuple_item(result, 1)
        _emit_internal_event(
            self,
            "hicache_internal.init_load_back",
            "load_back_request_observed",
            {
                "request_id": _jsonable_compact(_first_attr(getattr(params, "req", None), ("rid",))),
                "host_hit_length": _safe_int(getattr(params, "host_hit_length", None)),
                "mem_quota": _safe_int(getattr(params, "mem_quota", None)),
                "best_match_node": _summary_or_none(best_match_node),
                "best_match_node_chain": _chain_or_empty(best_match_node),
                "loaded_tokens": _base._safe_len(loaded_values),
                "last_node": _summary_or_none(last_node),
                "last_node_chain": _chain_or_empty(last_node),
                "queue_before": before,
                "queue_after": _async_queue_snapshot(self),
            },
            target="HiRadixCache.init_load_back",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_load_back(method: Any) -> Any:
    """包装 load-back 结果，记录 node 从 host 回载到 device 的证据。"""

    @functools.wraps(method)
    def wrapped(self: Any, node: Any, *args: Any, **kwargs: Any) -> Any:
        before = _async_queue_snapshot(self)
        before_node = _node_summary_record(node)
        result = method(self, node, *args, **kwargs)
        _emit_internal_event(
            self,
            "hicache_internal.load_back",
            "load_back_result_observed",
            {
                "mem_quota": _safe_int(_arg_or_kw(args, kwargs, 0, "mem_quota")),
                "before_node": before_node,
                "after_node": _node_summary_record(node),
                "node_chain": _node_chain_record(node),
                "loaded_tokens": _base._safe_len(result),
                "queue_before": before,
                "queue_after": _async_queue_snapshot(self),
                "evictable": _evictable_snapshot_record(self),
            },
            target="HiRadixCache.load_back",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_write_backup(method: Any) -> Any:
    """包装 write_backup 调度，记录待写回 node 和队列变化。"""

    @functools.wraps(method)
    def wrapped(self: Any, node: Any, *args: Any, **kwargs: Any) -> Any:
        before = _async_queue_snapshot(self)
        result = method(self, node, *args, **kwargs)
        _emit_internal_event(
            self,
            "hicache_internal.write_backup",
            "writeback_schedule_observed",
            {
                "node": _node_summary_record(node),
                "node_chain": _node_chain_record(node),
                "write_back": bool(_arg_or_kw(args, kwargs, 0, "write_back")),
                "written_tokens": _safe_int(result),
                "queue_before": before,
                "queue_after": _async_queue_snapshot(self),
                "evictable": _evictable_snapshot_record(self),
            },
            target="HiRadixCache.write_backup",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_write_backup_storage(method: Any) -> Any:
    """包装 storage write-back 调度，记录异步队列快照。"""

    @functools.wraps(method)
    def wrapped(self: Any, node: Any, *args: Any, **kwargs: Any) -> Any:
        before = _async_queue_snapshot(self)
        result = method(self, node, *args, **kwargs)
        _emit_internal_event(
            self,
            "hicache_internal.write_backup_storage",
            "writeback_storage_schedule_observed",
            {
                "node": _node_summary_record(node),
                "node_chain": _node_chain_record(node),
                "queue_before": before,
                "queue_after": _async_queue_snapshot(self),
            },
            target="HiRadixCache.write_backup_storage",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_evict_host(method: Any) -> Any:
    """包装 host eviction，记录请求和实际释放 token 数。"""

    @functools.wraps(method)
    def wrapped(self: Any, *args: Any, **kwargs: Any) -> Any:
        before = _evictable_snapshot_record(self)
        result = method(self, *args, **kwargs)
        _emit_internal_event(
            self,
            "hicache_internal.host_eviction",
            "host_eviction_observed",
            {
                "requested_tokens": _safe_int(_arg_or_kw(args, kwargs, 0, "num_tokens")),
                "evicted_tokens": _safe_int(result),
                "evictable_before": before,
                "evictable_after": _evictable_snapshot_record(self),
            },
            target="HiRadixCache.evict_host",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_hiradix_terminate_prefetch(method: Any) -> Any:
    """包装 HiRadixCache prefetch 终止请求。"""

    @functools.wraps(method)
    def wrapped(self: Any, req_id: Any, *args: Any, **kwargs: Any) -> Any:
        before = _prefetch_progress_record_for_req(self, req_id, None)
        result = method(self, req_id, *args, **kwargs)
        _emit_internal_event(
            self,
            "hicache_internal.hiradix_prefetch_terminate",
            "prefetch_terminate_requested_observed",
            {
                "request_id": _jsonable_compact(req_id),
                "before": before,
                "after": _prefetch_progress_record_for_req(self, req_id, result),
                "queue_snapshot": _async_queue_snapshot(self),
            },
            target="HiRadixCache.terminate_prefetch",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_release_aborted_request(method: Any) -> Any:
    """包装 abort cleanup，记录 request 相关 prefetch 状态收敛。"""

    @functools.wraps(method)
    def wrapped(self: Any, rid: Any, *args: Any, **kwargs: Any) -> Any:
        before = _prefetch_progress_record_for_req(self, rid, None)
        result = method(self, rid, *args, **kwargs)
        _emit_internal_event(
            self,
            "hicache_internal.release_aborted_request",
            "request_abort_cleanup_observed",
            {
                "request_id": _jsonable_compact(rid),
                "before": before,
                "after": _prefetch_progress_record_for_req(self, rid, result),
                "queue_snapshot": _async_queue_snapshot(self),
            },
            target="HiRadixCache.release_aborted_request",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_pop_prefetch_loaded_tokens(method: Any) -> Any:
    """包装已加载 prefetch token 消费点。"""

    @functools.wraps(method)
    def wrapped(self: Any, req_id: Any, *args: Any, **kwargs: Any) -> Any:
        before = _prefetch_progress_record_for_req(self, req_id, None)
        result = method(self, req_id, *args, **kwargs)
        _emit_internal_event(
            self,
            "hicache_internal.pop_prefetch_loaded_tokens",
            "prefetch_loaded_tokens_observed",
            {
                "request_id": _jsonable_compact(req_id),
                "loaded_tokens": _safe_int(result),
                "before": before,
                "after": _prefetch_progress_record_for_req(self, req_id, result),
            },
            target="HiRadixCache.pop_prefetch_loaded_tokens",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_writing_check(method: Any) -> Any:
    """包装 write ack 轮询点，记录写回队列前后状态。"""

    @functools.wraps(method)
    def wrapped(self: Any, *args: Any, **kwargs: Any) -> Any:
        before = _async_queue_snapshot(self)
        result = method(self, *args, **kwargs)
        after = _async_queue_snapshot(self)
        if before != after or before.get("ongoing_write_through", 0) > 0 or probe_debug_enabled():
            _emit_internal_event(
                self,
                "hicache_internal.write_ack_checkpoint",
                "write_ack_checkpoint_observed",
                {
                    "write_back": bool(args[0]) if args else bool(kwargs.get("write_back", False)),
                    "before": before,
                    "after": after,
                },
                target="HiRadixCache.writing_check",
                fact_class="source_actual",
            )
        return result

    return wrapped


def _wrap_loading_check(method: Any) -> Any:
    """包装 load ack 轮询点，记录回载队列前后状态。"""

    @functools.wraps(method)
    def wrapped(self: Any, *args: Any, **kwargs: Any) -> Any:
        before = _async_queue_snapshot(self)
        result = method(self, *args, **kwargs)
        after = _async_queue_snapshot(self)
        if before != after or before.get("ongoing_load_back", 0) > 0 or probe_debug_enabled():
            _emit_internal_event(
                self,
                "hicache_internal.load_ack_checkpoint",
                "load_ack_checkpoint_observed",
                {"before": before, "after": after},
                target="HiRadixCache.loading_check",
                fact_class="source_actual",
            )
        return result

    return wrapped


def _wrap_drain_storage_control_queues(method: Any) -> Any:
    """包装 storage control queue drain，记录维护性队列变化。"""

    @functools.wraps(method)
    def wrapped(self: Any, *args: Any, **kwargs: Any) -> Any:
        before = _async_queue_snapshot(self)
        result = method(self, *args, **kwargs)
        after = _async_queue_snapshot(self)
        if before != after or probe_debug_enabled():
            _emit_internal_event(
                self,
                "hicache_internal.storage_control_checkpoint",
                "storage_control_checkpoint_observed",
                {"before": before, "after": after},
                target="HiRadixCache.drain_storage_control_queues",
                fact_class="source_actual",
            )
        return result

    return wrapped


def _wrap_controller_load(method: Any) -> Any:
    """包装 controller load 入队，记录 host/device token 数。"""

    @functools.wraps(method)
    def wrapped(self: Any, *args: Any, **kwargs: Any) -> Any:
        before = _controller_queue_snapshot(self)
        result = method(self, *args, **kwargs)
        host_indices = _arg_or_kw(args, kwargs, 0, "host_indices")
        _emit_internal_event(
            self,
            "hicache_internal.load_enqueue",
            "load_enqueue_observed",
            {
                "node_id": _jsonable_compact(_arg_or_kw(args, kwargs, 2, "node_id")),
                "host_tokens": _base._safe_len(host_indices),
                "device_tokens": _base._safe_len(result),
                "before": before,
                "after": _controller_queue_snapshot(self),
            },
            target="HiCacheController.load",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_controller_start_loading(method: Any) -> Any:
    """包装 controller load 启动，记录 producer 和队列状态。"""

    @functools.wraps(method)
    def wrapped(self: Any, *args: Any, **kwargs: Any) -> Any:
        before = _controller_queue_snapshot(self)
        result = method(self, *args, **kwargs)
        _emit_internal_event(
            self,
            "hicache_internal.load_start",
            "load_start_observed",
            {
                "producer_id": _jsonable_compact(result),
                "before": before,
                "after": _controller_queue_snapshot(self),
            },
            target="HiCacheController.start_loading",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_controller_write(method: Any) -> Any:
    """包装 controller write 入队，记录 device 到 host 的 token 迁移。"""

    @functools.wraps(method)
    def wrapped(self: Any, *args: Any, **kwargs: Any) -> Any:
        before = _controller_queue_snapshot(self)
        device_indices = _arg_or_kw(args, kwargs, 0, "device_indices")
        result = method(self, *args, **kwargs)
        _emit_internal_event(
            self,
            "hicache_internal.write_enqueue",
            "write_enqueue_observed",
            {
                "node_id": _jsonable_compact(_arg_or_kw(args, kwargs, 2, "node_id")),
                "device_tokens": _base._safe_len(device_indices),
                "host_tokens": _base._safe_len(result),
                "before": before,
                "after": _controller_queue_snapshot(self),
            },
            target="HiCacheController.write",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_controller_start_writing(method: Any) -> Any:
    """包装 controller write 启动，记录写队列推进。"""

    @functools.wraps(method)
    def wrapped(self: Any, *args: Any, **kwargs: Any) -> Any:
        before = _controller_queue_snapshot(self)
        result = method(self, *args, **kwargs)
        _emit_internal_event(
            self,
            "hicache_internal.write_start",
            "write_start_observed",
            {
                "before": before,
                "after": _controller_queue_snapshot(self),
            },
            target="HiCacheController.start_writing",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_controller_prefetch(method: Any) -> Any:
    """包装 controller prefetch 入队，记录 operation 和队列快照。"""

    @functools.wraps(method)
    def wrapped(self: Any, *args: Any, **kwargs: Any) -> Any:
        result = method(self, *args, **kwargs)
        request_id = _arg_or_kw(args, kwargs, 0, "request_id")
        host_indices = _arg_or_kw(args, kwargs, 1, "host_indices")
        new_input_tokens = _arg_or_kw(args, kwargs, 2, "new_input_tokens")
        _emit_internal_event(
            self,
            "hicache_internal.prefetch_enqueue",
            "prefetch_enqueue_observed",
            {
                "request_id": _jsonable_compact(request_id),
                "host_tokens": _base._safe_len(host_indices),
                "new_input_tokens": _base._safe_len(new_input_tokens),
                "operation": _storage_operation_record(result),
                "queue_snapshot": _controller_queue_snapshot(self),
            },
            target="HiCacheController.prefetch",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_prefetch_rate_limited(method: Any) -> Any:
    """包装 prefetch rate-limit 判断，记录 capacity limit 证据。"""

    @functools.wraps(method)
    def wrapped(self: Any, *args: Any, **kwargs: Any) -> Any:
        result = method(self, *args, **kwargs)
        _emit_internal_event(
            self,
            "hicache_internal.prefetch_rate_limit",
            "prefetch_rate_limit_observed",
            {
                "rate_limited": bool(result),
                "queue_snapshot": _controller_queue_snapshot(self),
                "prefetch_capacity_limit": _safe_int(getattr(self, "prefetch_capacity_limit", None)),
            },
            target="HiCacheController.prefetch_rate_limited",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_storage_hit_query(method: Any) -> Any:
    """包装 storage hit 查询，记录命中的 hash pages 和 token 数。"""

    @functools.wraps(method)
    def wrapped(self: Any, operation: Any, *args: Any, **kwargs: Any) -> Any:
        result = method(self, operation, *args, **kwargs)
        hash_value, storage_hit_count = result if isinstance(result, tuple) and len(result) == 2 else (None, None)
        _emit_internal_event(
            self,
            "hicache_internal.storage_hit_query",
            "storage_hit_query_observed",
            {
                "operation": _storage_operation_record(operation),
                "hit_hash_pages": _jsonable_compact(hash_value),
                "hit_page_count": _base._safe_len(hash_value),
                "hit_tokens": _safe_int(storage_hit_count),
                "queue_snapshot": _controller_queue_snapshot(self),
            },
            target="HiCacheController._storage_hit_query",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_terminate_prefetch(method: Any) -> Any:
    """包装 controller prefetch 终止，记录完成 token 和 hash pages。"""

    @functools.wraps(method)
    def wrapped(self: Any, operation: Any, *args: Any, **kwargs: Any) -> Any:
        result = method(self, operation, *args, **kwargs)
        completed_tokens, hash_value = result if isinstance(result, tuple) and len(result) == 2 else (None, None)
        _emit_internal_event(
            self,
            "hicache_internal.prefetch_terminate",
            "prefetch_terminate_observed",
            {
                "operation": _storage_operation_record(operation),
                "completed_tokens": _safe_int(completed_tokens),
                "hash_pages": _jsonable_compact(hash_value),
                "queue_snapshot": _controller_queue_snapshot(self),
            },
            target="HiCacheController.terminate_prefetch",
            fact_class="source_actual",
        )
        return result

    return wrapped


def _wrap_append_host_mem_release(method: Any) -> Any:
    """包装 host memory release 入队，记录 cleanup request 的目标规模。"""

    @functools.wraps(method)
    def wrapped(self: Any, host_indices: Any, *args: Any, **kwargs: Any) -> Any:
        before = _controller_queue_snapshot(self)
        result = method(self, host_indices, *args, **kwargs)
        after = _controller_queue_snapshot(self)
        if _base._safe_len(host_indices) or before != after or probe_debug_enabled():
            _emit_internal_event(
                self,
                "hicache_internal.host_mem_release_enqueue",
                "host_mem_release_enqueue_observed",
                {
                    "host_tokens": _base._safe_len(host_indices),
                    "before": before,
                    "after": after,
                },
                target="HiCacheController.append_host_mem_release",
                fact_class="source_actual",
            )
        return result

    return wrapped


def _emit_internal_event(
    cache: Any,
    target_id: str,
    event_role: str,
    payload: dict[str, Any],
    *,
    target: str,
    fact_class: str,
    dag_input: bool = False,
    model_input: bool = False,
) -> None:
    """发出 HiCache 内部 source_actual/debug 事件。

    默认 `model_input=false`、`dag_input=false`，确保 source actual 只用于质量
    审计和 oracle 对齐，不会被正常 C++ state model 当作输入事实消费。
    """

    try:
        timestamp = get_writer().now_us()
        scope, seq_no = _next_scope_seq(cache)
        get_writer().duration_event(
            f"hicache_{event_role}",
            timestamp,
            timestamp,
            "python_probe",
            {
                "schema_version": 1,
                "domain": "python_probe",
                "target_id": target_id,
                "target": target,
                "phase": "instant",
                "status": "completed",
                "missing_required_fields": [],
                "model_input": model_input,
                "event_kind": f"hicache_{event_role}",
                "dag_input": dag_input,
                "fact_class": fact_class,
                "event_role": event_role,
                "fact_granularity": "atomic",
                "cache_scope": scope,
                "seq_no": seq_no,
                "source_page_size": _safe_int(getattr(cache, "page_size", None)),
                **payload,
            },
        )
    except Exception as exc:
        if probe_debug_enabled():
            print(f"[trace_sim_probe] failed to emit HiCache internal event {target_id}: {exc}", flush=True)


def _next_scope_seq(cache: Any) -> tuple[str, int]:
    """为同一 cache_scope 分配单调递增 seq_no。"""

    scope = _cache_scope_key(cache)
    next_seq = _HICACHE_SEQUENCE_BY_SCOPE.get(scope, 0) + 1
    _HICACHE_SEQUENCE_BY_SCOPE[scope] = next_seq
    return scope, next_seq


def _set_contains(values: Any, item: Any) -> bool:
    """安全判断集合是否包含对象，兼容自定义容器异常。"""

    try:
        return item in values if values is not None else False
    except Exception:
        return False


def _medium_name(value: Any, *, default: str = "") -> str:
    """把 SGLang medium enum/对象规整成短字符串。"""

    if value is None:
        return default
    name = getattr(value, "name", None)
    if name:
        return str(name)
    text = str(value)
    return text.rsplit(".", 1)[-1] if "." in text else text


def _queue_size(value: Any) -> int | None:
    """读取 queue-like 对象长度，优先使用 qsize。"""

    qsize = getattr(value, "qsize", None)
    if callable(qsize):
        try:
            return _safe_int(qsize())
        except Exception:
            return None
    return _base._safe_len(value)


def _async_queue_snapshot(cache: Any) -> dict[str, Any]:
    """采集 HiRadixCache 异步写回/回载/prefetch 队列状态。"""

    controller = _first_attr(cache, ("cache_controller", "controller")) or cache
    return {
        "ongoing_write_through": _base._safe_len(getattr(cache, "ongoing_write_through", None)) or 0,
        "ongoing_load_back": _base._safe_len(getattr(cache, "ongoing_load_back", None)) or 0,
        "ongoing_prefetch": _base._safe_len(getattr(cache, "ongoing_prefetch", None)) or 0,
        "ongoing_backup": _base._safe_len(getattr(cache, "ongoing_backup", None)) or 0,
        "ack_write_queue": _base._safe_len(getattr(controller, "ack_write_queue", None)) or 0,
        "ack_load_queue": _base._safe_len(getattr(controller, "ack_load_queue", None)) or 0,
        "prefetch_revoke_queue": _queue_size(getattr(controller, "prefetch_revoke_queue", None)) or 0,
        "ack_backup_queue": _queue_size(getattr(controller, "ack_backup_queue", None)) or 0,
        "host_mem_release_queue": _queue_size(getattr(controller, "host_mem_release_queue", None)) or 0,
        "prefetch_tokens_occupied": _safe_int(getattr(controller, "prefetch_tokens_occupied", None)),
    }


def _controller_queue_snapshot(controller: Any) -> dict[str, Any]:
    """采集 HiCacheController 队列状态。"""

    return {
        "load_queue": _base._safe_len(getattr(controller, "load_queue", None)) or 0,
        "write_queue": _base._safe_len(getattr(controller, "write_queue", None)) or 0,
        "prefetch_queue": _queue_size(getattr(controller, "prefetch_queue", None)) or 0,
        "backup_queue": _queue_size(getattr(controller, "backup_queue", None)) or 0,
        "prefetch_revoke_queue": _queue_size(getattr(controller, "prefetch_revoke_queue", None)) or 0,
        "ack_backup_queue": _queue_size(getattr(controller, "ack_backup_queue", None)) or 0,
        "host_mem_release_queue": _queue_size(getattr(controller, "host_mem_release_queue", None)) or 0,
        "prefetch_tokens_occupied": _safe_int(getattr(controller, "prefetch_tokens_occupied", None)),
    }


def _storage_operation_record(operation: Any) -> dict[str, Any] | None:
    """把 storage operation 对象收敛为 JSON 摘要。"""

    if operation is None:
        return None
    return {
        "operation_id": _jsonable_compact(getattr(operation, "id", None)),
        "request_id": _jsonable_compact(getattr(operation, "request_id", None)),
        "token_count": _base._safe_len(getattr(operation, "token_ids", None)),
        "host_tokens": _base._safe_len(getattr(operation, "host_indices", None)),
        "device_tokens": _base._safe_len(getattr(operation, "device_indices", None)),
        "completed_tokens": _safe_int(getattr(operation, "completed_tokens", None)),
        "hash_value": _jsonable_compact(getattr(operation, "hash_value", None)),
        "prefix_keys": _jsonable_compact(getattr(operation, "prefix_keys", None)),
        "terminated": _safe_call_bool(operation, "is_terminated"),
    }


def _prefetch_progress_record_for_req(cache: Any, req_id: Any, result: Any) -> dict[str, Any]:
    """复用 prefetch progress extractor 生成某个 request 的进度记录。"""

    found, value = _extract_prefetch_progress(
        "arg:cache,arg:req_id",
        {"cache": cache, "req_id": req_id},
        (),
        {},
        result,
    )
    return value if found else {"request_id": _jsonable_compact(req_id)}


def _tuple_item(value: Any, index: int) -> Any:
    """安全读取 tuple/list 中的指定元素。"""

    if isinstance(value, (tuple, list)) and index < len(value):
        return value[index]
    return None


def _arg_or_kw(args: tuple[Any, ...], kwargs: dict[str, Any], index: int, key: str) -> Any:
    """按位置或关键字读取 wrapper 参数。"""

    if key in kwargs:
        return kwargs[key]
    if index < len(args):
        return args[index]
    return None


_base.register_source_extractor(_hicache_state_source)
_base.register_source_extractor(_token_path_source)
_base.register_source_extractor(_token_span_source)
_base.register_source_extractor(_request_token_path_source)
_base.register_source_extractor(_request_token_span_source)
_base.register_source_extractor(_request_token_count_source)
_base.register_source_extractor(_token_path_concat_source)
_base.register_source_extractor(_token_span_concat_source)
_base.register_source_extractor(_node_token_path_source)
_base.register_source_extractor(_node_token_span_source)
_base.register_source_extractor(_node_token_count_source)
_base.register_source_extractor(_hicache_node_summary_source)
_base.register_source_extractor(_hicache_node_chain_source)
_base.register_source_extractor(_hicache_evictable_snapshot_source)
_base.register_source_extractor(_hicache_prefetch_progress_source)
_base.register_source_extractor(_hicache_request_runtime_source)
_base.register_source_extractor(_hicache_scheduler_prefetch_state_source)
_base.register_source_extractor(_node_token_path_concat_source)
_base.register_source_extractor(_node_token_span_concat_source)
_base.register_source_extractor(_hicache_cache_scope_source)
_base.register_source_extractor(_hicache_seq_source)
_base.register_source_extractor(_hicache_config_source)
_base.register_source_extractor(_hicache_requested_pages_source)

TARGET_MODULES = tuple(sorted(set(_base.TARGET_MODULES) | set(_INTERNAL_TARGET_MODULES)))
