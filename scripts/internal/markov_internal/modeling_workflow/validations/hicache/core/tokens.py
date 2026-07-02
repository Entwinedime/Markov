"""HiCache token path 合同共享辅助工具。"""

from __future__ import annotations

import hashlib
import json
from typing import Any

TokenUnit = tuple[int, ...]
TokenPath = tuple[TokenUnit, ...]


def maybe_json(value: Any) -> Any:
    """解析 trace 中以字符串保存的嵌套 JSON 值。"""

    if isinstance(value, str):
        text = value.strip()
        if text.startswith("{") or text.startswith("["):
            try:
                return json.loads(text)
            except json.JSONDecodeError:
                return value
    return value


def token_id_path(value: Any) -> TokenPath | None:
    """解析 token_dictionary.token_ids，同时保留 composite token 结构。"""

    value = maybe_json(value)
    if not isinstance(value, dict):
        return None
    raw_tokens = value.get("token_ids")
    if not isinstance(raw_tokens, list):
        return None

    parsed: list[TokenUnit] = []
    for token in raw_tokens:
        try:
            if isinstance(token, (list, tuple)):
                parsed.append(tuple(int(item) for item in token))
            else:
                parsed.append((int(token),))
        except (TypeError, ValueError):
            return None
    return tuple(parsed)


def token_path_count(tokens: TokenPath) -> int:
    """返回解析后 token path 表示的模型 token 数。"""

    return len(tokens)


def token_path_hash(tokens: TokenPath) -> str:
    """用 probe 相同的 u32le 算法计算 token path hash。"""

    hasher = hashlib.sha256()
    for token in tokens:
        for item in token:
            hasher.update(int(item).to_bytes(4, byteorder="little", signed=False))
    return "sha256_u32le:" + hasher.hexdigest()


def token_dictionary_issues(value: Any) -> list[dict[str, Any]]:
    """在 token_ids 存在时校验 token_count 与 token_path_id。"""

    value = maybe_json(value)
    if not isinstance(value, dict) or "token_ids" not in value:
        return []

    path_id = str(value.get("token_path_id") or value.get("path_id") or "")
    declared_count = _int_or_none(value.get("token_count"))
    tokens = token_id_path(value)
    if tokens is None:
        return [{"issue": "token_dictionary_token_ids_invalid", "path_id": path_id}]

    issues: list[dict[str, Any]] = []
    actual_count = token_path_count(tokens)
    if declared_count is not None and declared_count != actual_count:
        issues.append(
            {
                "issue": "token_dictionary_token_count_mismatch",
                "path_id": path_id,
                "token_count": declared_count,
                "token_ids": actual_count,
            }
        )
    if path_id:
        computed_path_id = token_path_hash(tokens)
        if computed_path_id != path_id:
            issues.append(
                {
                    "issue": "token_dictionary_hash_mismatch",
                    "path_id": path_id,
                    "computed_path_id": computed_path_id,
                }
            )
    return issues


def _int_or_none(value: Any) -> int | None:
    """宽松解析整数；None、bool 或非法值按缺失处理。"""

    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None
