"""Compact comparison between predicted and target-observed HiCache effect shape."""

from __future__ import annotations

from typing import Any

from .shape_facts import scoped_resource_lane


ACTIVE_STATES = {"required", "partial"}
ARRIVAL_SCHEDULE_SENSITIVE_EFFECTS = {"loadback", "prefetch_visibility_dependency"}


def shapes_match(shape: dict[str, Any], oracle: dict[str, Any]) -> bool:
    """Admit only exact effect work and common-lane order for historical replay.

    Schedule-sensitive differences are still differences. The retired diagnostic
    classification never granted admission and is not needed to decide this.
    """
    predicted = {str(row["effect_key"]): row for row in shape["effects"]}
    actual = {
        str(row["effect_key"]): row
        for row in oracle.get("effects", [])
        if isinstance(row, dict) and row.get("effect_key")
    }
    if oracle.get("ready") is not True or not predicted or predicted.keys() != actual.keys():
        return False

    fields = (
        "direction",
        "consumer_role",
        "blocking_relation",
        "schedule_sensitivity",
        "operation_count",
        "effective_page_count",
        "completed_page_count",
        "storage_existing_page_count",
        "storage_new_page_count",
    )
    for key, expected in predicted.items():
        observed = actual[key]
        if expected.get("target_effect_state") != observed.get("actual_state"):
            return False
        if any(expected.get(field) != observed.get(field) for field in fields):
            return False
        if [tuple(batch) for batch in expected.get("storage_batches") or []] != [
            tuple(batch) for batch in observed.get("storage_batches") or []
        ]:
            return False

    predicted_orders = shape["lane_orders"]
    actual_orders = oracle.get("lane_orders") if isinstance(oracle.get("lane_orders"), dict) else {}
    for lane in predicted_orders.keys() | actual_orders.keys():
        expected, observed = predicted_orders.get(lane, []), actual_orders.get(lane, [])
        common = set(expected) & set(observed)
        if [key for key in expected if key in common] != [key for key in observed if key in common]:
            return False
    return True


def actual_row(
    opportunity: dict[str, Any],
    state: str,
    *,
    blocker: str = "",
    operation_sort_key: tuple[Any, ...] | None = None,
    actual_consumer_role: str = "",
    work: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Build one target-observed effect row from trace evidence."""

    return {
        "effect_key": opportunity["effect_key"],
        "effect_family_key": opportunity["effect_family_key"],
        "effect_type": opportunity["effect_type"],
        "actual_state": state,
        "direction": opportunity["direction"],
        "resource_lane": scoped_resource_lane(str(opportunity["cache_scope"]), str(opportunity["resource_lane"])),
        "schedule_sensitivity": schedule_sensitivity(str(opportunity["effect_type"])),
        "blocker": blocker,
        "operation_sort_key": operation_sort_key,
        "actual_consumer_role": actual_consumer_role,
        "operation_count": 0,
        "effective_page_count": 0,
        "completed_page_count": 0,
        "storage_existing_page_count": 0,
        "storage_new_page_count": 0,
        "storage_batches": [],
        **(work or {}),
    }


def schedule_sensitivity(effect_type: str) -> str:
    return "arrival_schedule_sensitive" if effect_type in ARRIVAL_SCHEDULE_SENSITIVE_EFFECTS else "schedule_invariant"
