"""Run-local request identity shared by transition evidence producers."""

from __future__ import annotations

from typing import Any


def canonical_request_key(row: dict[str, Any]) -> str:
    """Build the conservative request key used by operation gates.

    A concrete request identifier takes precedence. Operation identity is a
    fallback for asynchronous evidence that lacks a request, and cache scope is
    always retained to prevent identifiers from colliding across processes.
    """

    request_id = str(row.get("request_id") or "")
    cache_scope = str(row.get("cache_scope") or "")
    operation_id = str(row.get("operation_id") or "")
    if request_id:
        return f"{cache_scope}:{request_id}"
    if operation_id:
        return f"{cache_scope}:operation:{operation_id}"
    return cache_scope
