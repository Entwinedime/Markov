"""HiCache probe token path、span 和 request token helper。"""

from __future__ import annotations

import hashlib
from typing import Any

from .common import (
    _extract_source_value,
    _safe_int,
    _scope_from_optional_source,
)
from .context import _TOKEN_HASH_ALGO, _TOKEN_PATHS_EMITTED_BY_SCOPE


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
    return (True, _token_path_record(_tokens_for_path(value), scope, _token_dictionary_bucket(bound)))


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
    return (True, _token_path_record(tokens, scope, _token_dictionary_bucket(bound)))


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


def _extract_request_list(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, list[Any]]:
    """按 source spec 读取 batch 当前 request 列表。"""

    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if not parts:
        return (False, [])
    found, value = _extract_source_value(parts[0], "request_list", bound, args, kwargs, result)
    if not found or value is None:
        return (False, [])
    if isinstance(value, list):
        return (True, value)
    if isinstance(value, tuple):
        return (True, list(value))
    try:
        return (True, list(value))
    except TypeError:
        return (False, [])


def _request_id(req: Any) -> str:
    """读取 request id，缺失时返回空字符串并交给合同检查报错。"""

    value = getattr(req, "rid", "")
    if value is None:
        return ""
    return str(value)


def _extract_request_token_records(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, list[tuple[Any, list[Any], str]]]:
    """读取 batch request token path，并保留与 `self.reqs` 相同的顺序。"""

    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if not parts:
        return (False, [])
    found, requests = _extract_request_list(parts[0], bound, args, kwargs, result)
    if not found:
        return (False, [])
    mode = parts[1] if len(parts) > 1 else "active"
    scope = _scope_from_optional_source(parts[2], bound, args, kwargs, result) if len(parts) > 2 else ""
    return (True, [(req, _request_tokens(req, mode), scope) for req in requests])


def _extract_request_token_path_records(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, list[Any]]:
    """为 batch request 生成 token dictionary 数组。"""

    found, rows = _extract_request_token_records(spec, bound, args, kwargs, result)
    if not found:
        return (False, [])
    bucket = _token_dictionary_bucket(bound)
    return (True, [_token_path_record(tokens, scope, bucket) for _req, tokens, scope in rows])


def _extract_request_token_span_records(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, list[Any]]:
    """为 batch request 生成 token span 数组。"""

    found, rows = _extract_request_token_records(spec, bound, args, kwargs, result)
    if not found:
        return (False, [])
    return (True, [_token_span_record(tokens, 0, len(tokens)) for _req, tokens, _scope in rows])


def _extract_request_token_count_records(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, list[int]]:
    """为 batch request 生成 token count 数组。"""

    found, rows = _extract_request_token_records(spec, bound, args, kwargs, result)
    if not found:
        return (False, [])
    return (True, [len(tokens) for _req, tokens, _scope in rows])


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


def _token_span_record(tokens: list[Any], begin: int, end: int) -> dict[str, Any]:
    """生成 token span 记录，引用同一 hash 算法下的 path id。"""

    return {
        "path_id": _token_path_id(tokens),
        "begin": begin,
        "end": end,
        "token_count": len(tokens),
        "hash_algo": _TOKEN_HASH_ALGO,
    }


def _token_path_record(tokens: list[Any], scope: str = "", bucket: str = "unknown") -> dict[str, Any]:
    """生成 token dictionary 记录，并在同一 scope 内只携带一次 token_ids。"""

    path_id = _token_path_id(tokens)
    row: dict[str, Any] = {
        "token_path_id": path_id,
        "token_count": len(tokens),
        "hash_algo": _TOKEN_HASH_ALGO,
    }
    scope_key = f"{bucket}:{scope or 'global'}"
    emitted = _TOKEN_PATHS_EMITTED_BY_SCOPE.setdefault(scope_key, set())
    if path_id not in emitted:
        emitted.add(path_id)
        row["token_ids"] = _jsonable_token_ids(tokens)
    return row


def _token_dictionary_bucket(bound: dict[str, Any]) -> str:
    """区分 state model 输入与诊断证据的 token dictionary 去重域。"""

    phase = str(bound.get("__trace_sim_phase") or "")
    consumers = set(bound.get("__trace_sim_fact_consumers") or ())
    if "hicache_state_model" in consumers and phase == "end":
        return "state_model_end"
    if "hicache_state_model" in consumers:
        return f"state_model_{phase or 'unknown'}"
    return "diagnostic"


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


def _tokens_for_path(value: Any) -> list[Any]:
    """读取完整 token 序列。

    通用 probe 的 `_safe_list` 会截断到 64 个元素，适合作摘要；token dictionary
    需要完整 token 序列，不能在这里截断。
    """

    if value is None:
        return []
    if hasattr(value, "tolist") and callable(value.tolist):
        value = value.tolist()
    elif hasattr(value, "raw_token_ids") and callable(value.raw_token_ids):
        try:
            return list(value)
        except Exception:
            return _tokens_for_path(value.raw_token_ids())
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
        return _request_fill_tokens(req)
    if normalized == "extend":
        return _request_extend_tokens(req)
    if normalized in ("prefetch", "prefetch_candidate"):
        return _request_prefetch_tokens(req)
    if normalized in ("origin_output", "full"):
        return _request_origin_output_tokens(req)
    if normalized in ("committed", "cache_committed"):
        tokens = _request_origin_output_tokens(req)
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
    fill = _request_fill_tokens(req)
    if fill:
        return fill
    return _request_origin_output_tokens(req)


def _request_fill_tokens(req: Any) -> list[Any]:
    """读取 `cache_unfinished_req()` 所用的当前 fill token 序列。"""

    get_fill_ids = getattr(req, "get_fill_ids", None)
    if callable(get_fill_ids):
        try:
            tokens = _tokens_for_path(get_fill_ids())
        except Exception:
            tokens = []
        if tokens:
            return tokens
    tokens = _tokens_for_path(getattr(req, "fill_ids", None))
    if tokens:
        return tokens
    return []


def _request_extend_tokens(req: Any) -> list[Any]:
    """读取 `ScheduleBatch.prepare_for_extend` 已接受的 fill path。"""

    return _request_fill_tokens(req)


def _request_prefetch_tokens(req: Any) -> list[Any]:
    """读取 `_prefetch_kvcache()` 做 storage prefetch 判定时使用的 token path。

    SGLang 当前实现会先 `init_next_round_input()`，再用
    `full_untruncated_fill_ids[:_compute_max_prefix_len(...)]` 作为 prefetch
    候选全集。这里不能复用 `get_fill_ids()`：forced-token replay 下 `fill_len`
    可能尚未初始化，而 full path 已经在 `full_untruncated_fill_ids` 中。
    """

    tokens = _tokens_for_path(getattr(req, "full_untruncated_fill_ids", None))
    if not tokens:
        tokens = _request_fill_tokens(req)
    if not tokens:
        tokens = _request_origin_output_tokens(req)

    compute_limit = getattr(req, "_compute_max_prefix_len", None)
    if callable(compute_limit):
        try:
            limit = _safe_int(compute_limit(len(tokens)))
        except Exception:
            limit = None
        if limit is not None:
            tokens = tokens[:limit]
    return tokens


def _request_origin_output_tokens(req: Any) -> list[Any]:
    """读取请求原始输入和已生成输出拼接后的完整 path。"""

    return _tokens_for_path(getattr(req, "origin_input_ids", None)) + _tokens_for_path(getattr(req, "output_ids", None))
