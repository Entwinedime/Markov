"""Compact comparison between predicted and target-observed HiCache effect shape."""

from __future__ import annotations

from collections import defaultdict
from typing import Any

from .shape_facts import nested_value, scoped_resource_lane, u64


ACTIVE_STATES = {"required", "partial"}
ARRIVAL_SCHEDULE_SENSITIVE_EFFECTS = {"loadback", "prefetch_visibility_dependency"}


def compare_predicted_shape(
    model_summary: dict[str, Any],
    oracle: dict[str, Any],
) -> dict[str, Any]:
    """Compare stable effect shape; target observations never change prediction."""

    decisions = _effect_decisions(model_summary)
    predicted_rows, predicted_lane_orders = _predicted_shape(decisions)
    predicted = {str(row["effect_key"]): row for row in predicted_rows}
    actual = {
        str(row.get("effect_key")): row
        for row in oracle.get("effects", [])
        if isinstance(row, dict) and row.get("effect_key")
    }
    blockers = []
    if oracle.get("ready") is not True:
        blockers.append("target_shape_oracle_not_ready")
    if not decisions:
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
    )
    for effect_key in sorted(predicted_keys & actual_keys):
        predicted_row = predicted[effect_key]
        actual_row = actual[effect_key]
        for predicted_field, actual_field, label in compared_fields:
            if predicted_row.get(predicted_field) == actual_row.get(actual_field):
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
    if (
        predicted_row.get("effect_type") == "prefetch_io_operation"
        and field in {"consumer_role", "blocking_relation"}
    ):
        return schedule_sensitivity("prefetch_visibility_dependency")
    return sensitivity


def actual_row(
    opportunity: dict[str, Any],
    state: str,
    *,
    blocker: str = "",
    operation_sort_key: tuple[Any, ...] | None = None,
    actual_consumer_role: str = "",
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
    }


def schedule_sensitivity(effect_type: str) -> str:
    return "arrival_schedule_sensitive" if effect_type in ARRIVAL_SCHEDULE_SENSITIVE_EFFECTS else "schedule_invariant"


def _predicted_shape(decisions: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], dict[str, list[str]]]:
    families: dict[str, dict[str, dict[str, Any]]] = defaultdict(dict)
    for decision in decisions:
        families[str(decision.get("effect_family_key") or "")][str(decision.get("effect_type") or "")] = decision
    rows: list[dict[str, Any]] = []
    lane_rows: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for decision in decisions:
        effect_type = str(decision.get("effect_type") or "")
        state = str(decision.get("target_effect_state") or "unresolved")
        consumer_role, blocking = _predicted_relation(
            effect_type,
            state,
            decision,
            families[str(decision.get("effect_family_key") or "")],
        )
        row = {
            "effect_key": str(decision.get("effect_key") or ""),
            "effect_family_key": str(decision.get("effect_family_key") or ""),
            "effect_type": effect_type,
            "target_effect_state": state,
            "direction": str(decision.get("direction") or ""),
            "consumer_role": consumer_role,
            "blocking_relation": blocking,
            "schedule_sensitivity": str(decision.get("schedule_sensitivity") or "unclassified"),
            "resource_lane": scoped_resource_lane(
                str(decision.get("cache_scope") or ""), str(decision.get("resource_lane") or "")
            ),
            "eligibility_epoch": u64(decision.get("eligibility_epoch")),
        }
        rows.append(row)
        if state in ACTIVE_STATES and row["resource_lane"]:
            lane_rows[row["resource_lane"]].append(row)
    lane_orders: dict[str, list[str]] = {}
    for lane, values in sorted(lane_rows.items()):
        values.sort(key=lambda row: (row["eligibility_epoch"], row["effect_key"]))
        lane_orders[lane] = [str(row["effect_key"]) for row in values]
    return rows, lane_orders


def _predicted_relation(
    effect_type: str, state: str, decision: dict[str, Any], family: dict[str, dict[str, Any]]
) -> tuple[str, str]:
    if state not in ACTIVE_STATES:
        return "none", "none"
    if effect_type == "loadback":
        return "foreground_cache_consumer", "blocking"
    if effect_type == "prefetch_io_operation":
        visibility = str(family.get("prefetch_visibility_dependency", {}).get("target_effect_state") or "")
        return ("prefetch_visibility_dependency" if visibility in ACTIVE_STATES else "background_completion"), "background"
    if effect_type == "prefetch_visibility_dependency":
        return str(nested_value(decision, "consumer_boundary", "source_fact_role") or "cache_extend_input"), "blocking"
    if effect_type == "commit_device_to_host":
        if str(family.get("commit_host_to_storage", {}).get("target_effect_state") or "") in ACTIVE_STATES:
            return "commit_host_to_storage", "family_internal"
        if str(family.get("commit_capacity_gate", {}).get("target_effect_state") or "") in ACTIVE_STATES:
            return "commit_capacity_gate", "family_internal"
        return "commit_completion", "family_internal"
    if effect_type == "commit_host_to_storage":
        capacity = str(family.get("commit_capacity_gate", {}).get("target_effect_state") or "")
        return ("commit_capacity_gate" if capacity in ACTIVE_STATES else "storage_completion"), "family_internal"
    if effect_type == "commit_capacity_gate":
        return str(decision.get("consumer_role") or "next_capacity_consumer"), "blocking"
    return "unknown", "unknown"


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
        "acceptance_ready": ready and invariant_mismatches == 0,
        "mismatch_count": mismatch_count,
        "acceptance_mismatch_count": invariant_mismatches,
        "schedule_sensitive_count": schedule_count,
        "diagnostic_exact": mismatch_count == 0,
    }


def _effect_decisions(model_summary: dict[str, Any]) -> list[dict[str, Any]]:
    modules = model_summary.get("modules") if isinstance(model_summary.get("modules"), list) else []
    for module in modules:
        if not isinstance(module, dict) or module.get("name") != "HiCacheModule":
            continue
        hicache = module.get("hicache") if isinstance(module.get("hicache"), dict) else {}
        ledger = hicache.get("effect_decisions") if isinstance(hicache.get("effect_decisions"), dict) else {}
        decisions = ledger.get("decisions") if isinstance(ledger.get("decisions"), list) else []
        return [decision for decision in decisions if isinstance(decision, dict)]
    return []
