"""Compact comparison between predicted and target-observed HiCache effect shape."""

from __future__ import annotations

from typing import Any

from .shape_facts import scoped_resource_lane


ACTIVE_STATES = {"required", "partial"}
ARRIVAL_SCHEDULE_SENSITIVE_EFFECTS = {"loadback", "prefetch_visibility_dependency"}


def compare_shape(shape: dict[str, Any], oracle: dict[str, Any]) -> dict[str, Any]:
    """Score the same projection whether detailed diagnostics were retained or not."""

    predicted_rows, predicted_lane_orders = shape["effects"], shape["lane_orders"]
    predicted = {str(row["effect_key"]): row for row in predicted_rows}
    actual = {
        str(row.get("effect_key")): row
        for row in oracle.get("effects", [])
        if isinstance(row, dict) and row.get("effect_key")
    }
    blockers = []
    if oracle.get("ready") is not True:
        blockers.append("target_shape_oracle_not_ready")
    if not predicted_rows:
        blockers.append("predicted_effect_decisions_missing")
    if blockers:
        return _comparison_result(
            ready=False,
            predicted_rows=predicted_rows,
            invariant_mismatches=0,
            schedule_mismatches=0,
        )

    predicted_keys = set(predicted)
    actual_keys = set(actual)
    invariant = len(predicted_keys ^ actual_keys)
    schedule_sensitive = 0

    compared_fields = (
        ("target_effect_state", "actual_state", "operation_presence"),
        ("direction", "direction", "transfer_direction"),
        ("consumer_role", "consumer_role", "consumer_role"),
        ("blocking_relation", "blocking_relation", "blocking_relation"),
        ("schedule_sensitivity", "schedule_sensitivity", "schedule_sensitivity"),
        ("operation_count", "operation_count", "operation_count"),
        ("effective_page_count", "effective_page_count", "effective_page_count"),
        ("completed_page_count", "completed_page_count", "completed_page_count"),
        ("storage_existing_page_count", "storage_existing_page_count", "storage_existing_page_count"),
        ("storage_new_page_count", "storage_new_page_count", "storage_new_page_count"),
        ("storage_batches", "storage_batches", "storage_batches"),
    )
    for effect_key in sorted(predicted_keys & actual_keys):
        predicted_row = predicted[effect_key]
        actual_row = actual[effect_key]
        for predicted_field, actual_field, label in compared_fields:
            predicted_value = predicted_row.get(predicted_field)
            actual_value = actual_row.get(actual_field)
            if label == "storage_batches":
                predicted_value = [tuple(batch) for batch in predicted_value or []]
                actual_value = [tuple(batch) for batch in actual_value or []]
            if predicted_value == actual_value:
                continue
            sensitivity = _field_schedule_sensitivity(predicted_row, label)
            if label != "transfer_direction" and sensitivity == "arrival_schedule_sensitive":
                schedule_sensitive += 1
            else:
                invariant += 1

    actual_lane_orders = oracle.get("lane_orders") if isinstance(oracle.get("lane_orders"), dict) else {}
    for lane in sorted(set(predicted_lane_orders) | set(actual_lane_orders)):
        predicted_lane = predicted_lane_orders.get(lane, [])
        actual_lane = actual_lane_orders.get(lane, [])
        common = set(predicted_lane) & set(actual_lane)
        predicted_common = [key for key in predicted_lane if key in common]
        actual_common = [key for key in actual_lane if key in common]
        if predicted_common == actual_common:
            continue
        sensitivities = {str(predicted[key].get("schedule_sensitivity") or "") for key in common}
        if sensitivities == {"arrival_schedule_sensitive"}:
            schedule_sensitive += 1
        else:
            invariant += 1

    return _comparison_result(
        ready=True,
        predicted_rows=predicted_rows,
        invariant_mismatches=invariant,
        schedule_mismatches=schedule_sensitive,
    )


def _field_schedule_sensitivity(predicted_row: dict[str, Any], field: str) -> str:
    """Assign ownership to relation fields whose value follows a sensitive sibling."""

    sensitivity = str(predicted_row.get("schedule_sensitivity") or "")
    if predicted_row.get("effect_type") == "prefetch_io_operation" and field in {"consumer_role", "blocking_relation"}:
        return schedule_sensitivity("prefetch_visibility_dependency")
    return sensitivity


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


def _comparison_result(
    *,
    ready: bool,
    predicted_rows: list[dict[str, Any]],
    invariant_mismatches: int,
    schedule_mismatches: int,
) -> dict[str, Any]:
    schedule_count = sum(row.get("schedule_sensitivity") == "arrival_schedule_sensitive" for row in predicted_rows)
    mismatch_count = invariant_mismatches + schedule_mismatches
    return {
        "acceptance_ready": ready and mismatch_count == 0,
        "schedule_conditioned_ready": ready and invariant_mismatches == 0,
        "mismatch_count": mismatch_count,
        "acceptance_mismatch_count": invariant_mismatches,
        "schedule_sensitive_count": schedule_count,
        "diagnostic_exact": mismatch_count == 0,
    }
