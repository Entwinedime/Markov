"""Role sets and records used by HiCache workload signatures."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any


DEFAULT_ROLES = (
    "cache_lookup_input",
    "cache_extend_input",
    "cache_lifecycle_commit",
    "prefetch_candidate_anchor",
)

PATH_ROLES = {
    "cache_lookup_input",
    "cache_extend_input",
    "cache_lifecycle_commit",
    "prefetch_candidate_anchor",
}

ROLE_SCALAR_FIELDS = {
    "cache_lookup_input": ("token_count",),
    "cache_lifecycle_commit": ("lifecycle_kind", "token_count", "priority"),
    "prefetch_candidate_anchor": ("token_count",),
    "cache_extend_input": ("batch_kind", "batch_size"),
}

KNOWN_WORKLOAD_IDENTITY_ROLES = set(DEFAULT_ROLES)
REQUEST_SCOPED_ROLES = {
    "cache_lookup_input",
    "cache_lifecycle_commit",
    "prefetch_candidate_anchor",
}


@dataclass(frozen=True)
class AuditEvent:
    """Comparable projection of one workload-identity fact."""

    stream: str
    ordinal: int
    role: str
    signature: str
    target_id: str
    event_name: str
    ts: int
    seq_no: int
    request_id: str
    request_fingerprint: str
    cache_scope: str
    fields: dict[str, Any]
