"""Shared HiCache token path contract helpers."""

from __future__ import annotations

import hashlib
import json
from typing import Any

TokenUnit = tuple[int, ...]
TokenPath = tuple[TokenUnit, ...]


def maybe_json(value: Any) -> Any:
    """Decode JSON-encoded values when traces stored nested fields as strings."""

    if isinstance(value, str):
        text = value.strip()
        if text.startswith("{") or text.startswith("["):
            try:
                return json.loads(text)
            except json.JSONDecodeError:
                return value
    return value


def token_id_path(value: Any) -> TokenPath | None:
    """Parse a token_dictionary token_ids field without losing composite tokens."""

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
    """Return the model token count represented by a parsed token path."""

    return len(tokens)


def token_path_hash(tokens: TokenPath) -> str:
    """Hash a token path with the same u32le algorithm used by the probe."""

    hasher = hashlib.sha256()
    for token in tokens:
        for item in token:
            hasher.update(int(item).to_bytes(4, byteorder="little", signed=False))
    return "sha256_u32le:" + hasher.hexdigest()


def token_dictionary_issues(value: Any) -> list[dict[str, Any]]:
    """Validate token_ids against token_count and token_path_id when token_ids exist."""

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
    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None
