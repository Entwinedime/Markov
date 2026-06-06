"""SGLang HiCache callable probe。

该 probe 复用 `generic_callable` 的函数包装逻辑，只补充 HiCache 建模需要的
`page_hashes:` 字段 source。这样通用 probe 不再包含 HiCache 特化规则。
"""

from __future__ import annotations

import hashlib
from typing import Any

from trace_sim_probe.probes import generic_callable as _base


def _page_hashes_source(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, bool, Any]:
    if not source.startswith("page_hashes:"):
        return (False, False, None)
    found, value = _extract_page_hashes(source.split(":", 1)[1], bound, args, kwargs, result)
    return (True, found, value)


def _extract_page_hashes(
    spec: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
    result: Any,
) -> tuple[bool, Any]:
    """按 SGLang HiCache page hash 规则采集 page identity。

    表达式格式为 `page_hashes:<tokens>,<page_size>[,<prior_hash>]`。
    `<tokens>` 可以是 RadixKey，也可以是 token id 序列；`<prior_hash>` 用于
    prefetch 这类从已有前缀继续计算 page hash 的场景。
    """

    parts = [part.strip() for part in spec.split(",") if part.strip()]
    if len(parts) < 2:
        return (False, None)
    found_tokens, tokens = _base._extract_raw_value(parts[0], "page_identity", bound, args, kwargs, result)
    found_page_size, page_size_value = _base._extract_raw_value(parts[1], "page_size", bound, args, kwargs, result)
    if not found_tokens or not found_page_size:
        return (False, None)
    page_size = _safe_int(page_size_value)
    if page_size is None or page_size <= 0:
        return (False, None)
    prior_hash = None
    if len(parts) >= 3:
        found_prior, prior_value = _base._extract_raw_value(parts[2], "prior_hash", bound, args, kwargs, result)
        if found_prior and prior_value:
            prior_hash = str(prior_value)
    return (True, _compute_page_hashes(tokens, page_size, prior_hash))


def _safe_int(value: Any) -> int | None:
    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _compute_page_hashes(value: Any, page_size: int, prior_hash: str | None) -> list[str]:
    """复用 RadixKey.hash_page；普通 token 序列按同样 SHA256 规则计算。"""

    if hasattr(value, "page_aligned") and hasattr(value, "hash_page"):
        key = value.page_aligned(page_size)
        hashes: list[str] = []
        parent_hash = prior_hash
        for start in range(0, len(key), page_size):
            end = min(start + page_size, len(key))
            if end <= start:
                continue
            parent_hash = key.hash_page(start, end, parent_hash)
            hashes.append(parent_hash)
        return hashes

    tokens = _base._safe_list(value) or []
    aligned_len = len(tokens) // page_size * page_size
    hashes = []
    parent_hash = prior_hash
    for start in range(0, aligned_len, page_size):
        parent_hash = _hash_token_page(tokens[start : start + page_size], parent_hash)
        hashes.append(parent_hash)
    return hashes


def _hash_token_page(tokens: list[Any], prior_hash: str | None) -> str:
    hasher = hashlib.sha256()
    if prior_hash:
        hasher.update(bytes.fromhex(prior_hash))
    for token in tokens:
        if isinstance(token, (list, tuple)):
            for item in token:
                hasher.update(int(item).to_bytes(4, byteorder="little", signed=False))
        else:
            hasher.update(int(token).to_bytes(4, byteorder="little", signed=False))
    return hasher.hexdigest()


_base.register_source_extractor(_page_hashes_source)

install = _base.install
TARGET_MODULES = _base.TARGET_MODULES
