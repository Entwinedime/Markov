"""Mapping from HiCache fact roles to diagnostic mechanism coverage."""

from __future__ import annotations

from collections import Counter
from typing import Any

from ..core.facts import parse_fact_or_none


ROLE_TO_MECHANISM = {
    "cache_lookup_input": "lookup",
    "cache_lifecycle_commit": "insert",
    "cache_extend_input": "cache_extend",
    "request_admission_observed": "admission",
    "insert_result_observed": "insert",
    "prefetch_candidate_anchor": "prefetch_schedule",
    "prefetch_decision_observed": "prefetch_schedule",
    "prefetch_intent_observed": "prefetch_schedule",
    "prefetch_progress_observed": "prefetch_progress",
    "capacity_request": "evict",
    "capacity_result_observed": "evict",
    "lock_scope_delta": "lock_ref",
    "lock_scope_result_observed": "lock_ref",
    "prefetch_io_observed": "prefetch_transfer",
    "writeback_io_observed": "write_storage",
    "writeback_enqueue_observed": "write_storage",
}


def observe_mechanism(counter: Counter[str], args: dict[str, Any]) -> None:
    """Count the mechanism represented by one completed fact."""

    fact = parse_fact_or_none(args)
    if fact is None or not completed_fact_role(args, fact.role):
        return
    mechanism = ROLE_TO_MECHANISM.get(fact.role)
    if mechanism:
        counter[mechanism] += 1


def configured_mechanisms(configured_targets: dict[str, dict[str, Any]]) -> list[str]:
    """Derive theoretically observable mechanisms from configured targets."""

    mechanisms: set[str] = set()
    for target in configured_targets.values():
        mechanism = ROLE_TO_MECHANISM.get(configured_fact_role(target))
        if mechanism:
            mechanisms.add(mechanism)
    return sorted(mechanisms)


def configured_fact_role(target: dict[str, Any]) -> str:
    """Read the declared fact role from one configured probe target."""

    fact = target.get("fact")
    if isinstance(fact, dict):
        role = fact.get("role")
        if isinstance(role, str):
            return role
    return ""


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
    return fact is not None and (fact.role in ROLE_TO_MECHANISM or fact.role in state_roles)
