"""Token dictionary checks for already decoded source probe facts."""

from __future__ import annotations

import hashlib
from typing import Any

UINT32_MAX = (1 << 32) - 1


def fact_items(value: Any) -> list[Any]:
    """Normalize a scalar or array-valued fact field to a list."""

    if isinstance(value, list):
        return value
    return [] if value is None else [value]


def token_dictionary_issue_count(value: dict[str, Any]) -> int:
    """Count invalid token data, declared length and path-identity mismatches.

    The source accumulator supplies a decoded dictionary with list-valued
    token_ids. Composite tokens retain their boundaries for the token count,
    while hashing uses the probe's flattened unsigned-32-bit little-endian data.
    This is trace token identity, not artifact version or freshness checking.
    """

    tokens = value["token_ids"]
    hasher = hashlib.sha256()
    for token in tokens:
        components = token if isinstance(token, (list, tuple)) else (token,)
        if not components:
            return 1
        for component in components:
            if isinstance(component, bool):
                return 1
            try:
                number = int(component)
            except (TypeError, ValueError):
                return 1
            if not 0 <= number <= UINT32_MAX:
                return 1
            hasher.update(number.to_bytes(4, byteorder="little", signed=False))

    declared = value.get("token_count")
    try:
        count = None if declared is None or isinstance(declared, bool) else int(declared)
    except (TypeError, ValueError):
        count = None

    issues = int(count is not None and count != len(tokens))
    path_id = str(value.get("token_path_id") or value.get("path_id") or "")
    if path_id and path_id != "sha256_u32le:" + hasher.hexdigest():
        issues += 1
    return issues
