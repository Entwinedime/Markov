"""Identify the HiCache fact roles consumed by the profile audit."""

from __future__ import annotations

from typing import Any

from ..core.facts import parse_fact_or_none


HICACHE_PROFILE_ROLES = {
    "cache_lookup_input",
    "cache_lifecycle_commit",
    "cache_extend_input",
    "request_admission_observed",
    "insert_result_observed",
    "prefetch_candidate_anchor",
    "prefetch_decision_observed",
    "prefetch_intent_observed",
    "prefetch_progress_observed",
    "capacity_request",
    "capacity_result_observed",
    "lock_scope_delta",
    "lock_scope_result_observed",
    "prefetch_io_observed",
    "loadback_decision_observed",
    "loadback_io_observed",
    "commit_device_to_host_enqueue_observed",
    "commit_device_to_host_io_observed",
    "commit_capacity_release_observed",
    "writeback_io_observed",
    "writeback_enqueue_observed",
}


def completed_fact_role(args: dict[str, Any], role: str) -> bool:
    """Return whether a role is at its contract-defined consumable phase."""

    phase = str(args.get("phase") or "").lower()
    if role == "cache_extend_input":
        return phase == "start"
    return phase == "end"


def is_hicache_profile_event(args: dict[str, Any], state_roles: set[str]) -> bool:
    """Identify events included in the HiCache-specific quality audit."""

    target_id = str(args.get("target_id") or "").lower()
    if target_id.startswith(("hiradix.", "hicache.", "hicache_controller.")):
        return True
    fact = parse_fact_or_none(args)
    return fact is not None and (fact.role in HICACHE_PROFILE_ROLES or fact.role in state_roles)
