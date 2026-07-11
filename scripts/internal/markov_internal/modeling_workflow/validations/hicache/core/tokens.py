"""Shared parsing and identity helpers for the HiCache token-path contract."""

from __future__ import annotations

import hashlib
import json
from typing import Any

TokenUnit = tuple[int, ...]
TokenPath = tuple[TokenUnit, ...]
UINT32_MAX = (1 << 32) - 1


def fact_items(value: Any) -> list[Any]:
    """Normalize a scalar or array-valued fact field to a list."""

    if isinstance(value, list):
        return value
    if value is None:
        return []
    return [value]


def maybe_json(value: Any) -> Any:
    """Decode a trace argument that may contain one or two JSON string layers."""

    if not isinstance(value, str):
        return value
    text = value.strip()
    if not text:
        return value
    try:
        decoded = json.loads(text)
        if isinstance(decoded, str):
            nested = decoded.strip()
            if nested.startswith(("{", "[")):
                return json.loads(nested)
        return decoded
    except json.JSONDecodeError:
        return value


def token_id_path(value: Any) -> TokenPath | None:
    """Parse ``token_dictionary.token_ids`` while preserving composite tokens.

    The C++ hash contract serializes every token component as unsigned 32-bit
    little-endian data. Values outside that domain are rejected here so quality
    audit reports an invalid dictionary instead of failing during hashing.
    """

    value = maybe_json(value)
    if not isinstance(value, dict):
        return None
    raw_tokens = value.get("token_ids")
    if not isinstance(raw_tokens, list):
        return None

    parsed: list[TokenUnit] = []
    for token in raw_tokens:
        unit = _token_unit_or_none(token)
        if unit is None:
            return None
        parsed.append(unit)
    return tuple(parsed)


def _token_unit_or_none(token: Any) -> TokenUnit | None:
    """Parse one scalar or composite token without leaking conversion errors."""

    raw_unit = token if isinstance(token, (list, tuple)) else (token,)
    try:
        unit = tuple(_u32_token_component(item) for item in raw_unit)
    except (TypeError, ValueError):
        return None
    return unit or None


def _u32_token_component(value: Any) -> int:
    """Parse one token component under the shared unsigned-32-bit contract."""

    if isinstance(value, bool):
        raise ValueError("boolean token components are invalid")
    parsed = int(value)
    if parsed < 0 or parsed > UINT32_MAX:
        raise ValueError("token component is outside the unsigned 32-bit range")
    return parsed


def token_path_count(tokens: TokenPath) -> int:
    """Return the number of model tokens represented by a parsed path."""

    return len(tokens)


def token_path_hash(tokens: TokenPath) -> str:
    """Hash a token path with the probe's unsigned-32-bit LE algorithm."""

    hasher = hashlib.sha256()
    for token in tokens:
        for item in token:
            hasher.update(int(item).to_bytes(4, byteorder="little", signed=False))
    return "sha256_u32le:" + hasher.hexdigest()


def token_dictionary_issues(value: Any) -> list[dict[str, Any]]:
    """Validate count and path identity when ``token_ids`` are available."""

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
    """Parse an integer while treating booleans and invalid values as absent."""

    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None
