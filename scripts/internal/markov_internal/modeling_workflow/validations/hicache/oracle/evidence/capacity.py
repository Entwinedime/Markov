"""Audit of target HiCache configuration against capacity oracle evidence."""

from __future__ import annotations

from typing import Any

from .capacity_recommendation import recommend_hicache_target_config
from .capacity_values import (
    HiCacheCapacityEvidence,
    normalize_policy_value,
    parse_int_or_none,
    unique_int_values,
    unique_policy_values,
)
from ..snapshot.state import (
    derived_hicache_state_from_snapshot,
    snapshot_is_completed_state,
    snapshot_is_hiradix_cache_state,
    snapshot_timeline_sort_key,
    union_hicache_states,
)


def extract_hicache_capacity_oracle_state(snapshots: list[dict[str, Any]]) -> dict[str, Any]:
    """Aggregate capacity and policy snapshots from oracle traces.

    This evidence explains validation results but does not participate in state
    exactness. It records observed pool sizes, availability, and policies so a
    cross-config prediction need not infer them from state occupancy.
    """

    evidence = HiCacheCapacityEvidence()
    for row in snapshots:
        snapshot = row.get("state_snapshot")
        if isinstance(snapshot, dict):
            evidence.observe_snapshot(snapshot)
    return evidence.as_payload()


def build_hicache_capacity_config_audit(
    capacity_oracle: dict[str, Any],
    target_config: dict[str, Any],
    oracle_final_counts: dict[str, int],
    oracle_observed_max_counts: dict[str, int] | None = None,
) -> dict[str, Any]:
    """Compare C++ target config with observed capacity and policy facts.

    This audit is diagnostic and does not directly gate ``validation_ready``.
    Raw pool capacity can exceed the effective HiCache budget, so a target below
    the observed pool is noteworthy but not inherently incorrect.
    """

    unique_values = (
        capacity_oracle.get("unique_values") if isinstance(capacity_oracle.get("unique_values"), dict) else {}
    )
    observed_max_counts = oracle_observed_max_counts or {}
    target = {
        "page_size": parse_int_or_none(target_config.get("page_size")),
        "l1_capacity_pages": parse_int_or_none(
            target_config.get("l1_capacity_pages", target_config.get("l1_capacity"))
        ),
        "l2_capacity_pages": parse_int_or_none(
            target_config.get("l2_capacity_pages", target_config.get("l2_capacity"))
        ),
        "write_policy": normalize_policy_value(target_config.get("write_policy")),
        "prefetch_policy": normalize_policy_value(target_config.get("prefetch_policy")),
        "prefetch_threshold_pages": parse_int_or_none(target_config.get("prefetch_threshold_pages")),
        "prefetch_capacity_limit_pages": parse_int_or_none(target_config.get("prefetch_capacity_limit_pages")),
    }
    comparisons = {
        "page_size": _compare_int_config(
            "page_size", target["page_size"], unique_int_values(unique_values, ["page_size"])
        ),
        "write_policy": _compare_policy_config(
            "write_policy", target["write_policy"], unique_policy_values(unique_values, ["write_policy"])
        ),
        "prefetch_policy": _compare_policy_config(
            "prefetch_policy", target["prefetch_policy"], unique_policy_values(unique_values, ["prefetch_policy"])
        ),
        "prefetch_threshold_pages": _compare_int_config(
            "prefetch_threshold_pages",
            target["prefetch_threshold_pages"],
            unique_int_values(unique_values, ["prefetch_threshold_pages", "thresholds.prefetch_threshold_pages"]),
        ),
        "prefetch_capacity_limit_pages": _compare_int_config(
            "prefetch_capacity_limit_pages",
            target["prefetch_capacity_limit_pages"],
            unique_int_values(
                unique_values, ["prefetch_capacity_limit_pages", "thresholds.prefetch_capacity_limit_pages"]
            ),
        ),
        "l1_capacity_pages": _compare_capacity_config(
            "l1_capacity_pages",
            target["l1_capacity_pages"],
            unique_int_values(unique_values, ["l1_capacity_pages", "l1_pool.capacity_pages"]),
            oracle_final_counts.get("l1_resident_pages"),
            observed_max_counts.get("l1_resident_pages"),
        ),
        "l2_capacity_pages": _compare_capacity_config(
            "l2_capacity_pages",
            target["l2_capacity_pages"],
            unique_int_values(unique_values, ["l2_capacity_pages", "l2_pool.capacity_pages"]),
            oracle_final_counts.get("l2_resident_pages"),
            observed_max_counts.get("l2_resident_pages"),
        ),
    }
    warnings: list[str] = []
    likely_errors: list[str] = []
    for field, comparison in comparisons.items():
        status = str(comparison.get("status") or "")
        final_status = str(comparison.get("final_count_status") or "")
        max_status = str(comparison.get("observed_max_status") or "")
        if (
            status in {"target_exceeds_observed_pool", "mismatch"}
            or final_status == "target_below_oracle_final_count"
            or max_status == "target_below_oracle_observed_max_count"
        ):
            likely_errors.append(field)
        elif status in {"target_below_observed_pool", "not_configured", "no_observed_value"}:
            warnings.append(field)
    return {
        "ready": bool(capacity_oracle.get("ready")) or bool(target_config),
        "oracle_capacity_ready": bool(capacity_oracle.get("ready")),
        "target_config_ready": bool(target_config),
        "target_config": target,
        "comparisons": comparisons,
        "recommended_target_config": recommend_hicache_target_config(
            unique_values,
            oracle_final_counts,
            observed_max_counts,
            target,
        ),
        "warning_fields": warnings,
        "likely_error_fields": likely_errors,
        "note": "This audit is diagnostic. A target capacity lower than observed pool capacity can be a valid effective budget, but target capacity below oracle final resident count is a likely configuration error.",
    }


def _compare_int_config(field: str, target_value: int | None, observed_values: list[int]) -> dict[str, Any]:
    """Compare one integer target setting with observed oracle values."""

    if target_value is None or target_value <= 0:
        status = "not_configured"
    elif not observed_values:
        status = "no_observed_value"
    elif target_value in observed_values:
        status = "match"
    else:
        status = "mismatch"
    return {
        "field": field,
        "target_value": target_value,
        "observed_values": observed_values,
        "status": status,
    }


def _compare_policy_config(field: str, target_value: str, observed_values: list[str]) -> dict[str, Any]:
    """Compare one policy target setting with observed oracle values."""

    if target_value in {"", "observed"}:
        status = "not_configured"
    elif not observed_values:
        status = "no_observed_value"
    elif target_value in observed_values:
        status = "match"
    else:
        status = "mismatch"
    return {
        "field": field,
        "target_value": target_value,
        "observed_values": observed_values,
        "status": status,
    }


def _compare_capacity_config(
    field: str,
    target_value: int | None,
    observed_values: list[int],
    oracle_final_count: int | None,
    oracle_observed_max_count: int | None,
) -> dict[str, Any]:
    """Compare configured capacity with pool, final, and peak occupancy."""

    status = _capacity_pool_status(target_value, observed_values)
    final_status = _capacity_count_status(target_value, oracle_final_count, "oracle_final_count")
    observed_max_status = _capacity_count_status(
        target_value,
        oracle_observed_max_count,
        "oracle_observed_max_count",
    )
    return {
        "field": field,
        "target_value": target_value,
        "observed_pool_values": observed_values,
        "oracle_final_count": oracle_final_count,
        "oracle_observed_max_count": oracle_observed_max_count,
        "status": status,
        "final_count_status": final_status,
        "observed_max_status": observed_max_status,
    }


def _capacity_pool_status(target_value: int | None, observed_values: list[int]) -> str:
    """Classify a target capacity against configured source-pool values."""

    if target_value is None or target_value <= 0:
        return "not_configured"
    if not observed_values:
        return "no_observed_value"
    if target_value in observed_values:
        return "matches_observed_pool"
    if target_value > max(observed_values):
        return "target_exceeds_observed_pool"
    return "target_below_observed_pool"


def _capacity_count_status(target_value: int | None, observed_count: int | None, label: str) -> str:
    """Classify target capacity against one oracle occupancy count."""

    if observed_count is None:
        return f"no_{label}"
    if target_value is None or target_value <= 0:
        return "not_configured"
    if target_value < observed_count:
        return f"target_below_{label}"
    if target_value == observed_count:
        return f"matches_{label}"
    return f"target_above_{label}"


def observed_max_derived_state_counts(snapshots: list[dict[str, Any]]) -> dict[str, int]:
    """Compute peak set sizes along the raw snapshot timeline.

    Final state does not represent peak capacity pressure. This projection
    updates each HiRadixCache object independently, unions visible process state,
    and records maxima used to detect a target budget below observed residency.
    """

    timeline: list[tuple[tuple[int, int, int], tuple[str, str, str], dict[str, Any]]] = []
    fallback_states: list[dict[str, Any]] = []
    for row in snapshots:
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
            continue
        if not snapshot_is_hiradix_cache_state(row, snapshot) or not snapshot_is_completed_state(row):
            continue
        state = derived_hicache_state_from_snapshot(snapshot)
        fallback_states.append(state)
        object_id = str(row.get("object_id") or snapshot.get("object_id") or "")
        if not object_id:
            continue
        key = (str(row.get("trace_path") or ""), str(row.get("pid") or ""), object_id)
        timeline.append((snapshot_timeline_sort_key(row), key, row))

    max_counts: dict[str, int] = {}
    if timeline:
        object_states: dict[tuple[str, str, str], dict[str, Any]] = {}
        for _sort_key, key, row in sorted(timeline, key=lambda item: item[0]):
            object_states[key] = derived_hicache_state_from_snapshot(row.get("state_snapshot", {}))
            _update_max_state_counts(max_counts, union_hicache_states(object_states.values()))
        return max_counts

    for state in fallback_states:
        _update_max_state_counts(max_counts, state)
    return max_counts


def _update_max_state_counts(max_counts: dict[str, int], state: dict[str, Any]) -> None:
    """Update peak counts with one set-valued state projection."""

    for key, value in state.items():
        if not isinstance(value, list):
            continue
        count = len({str(item) for item in value if item is not None})
        max_counts[str(key)] = max(max_counts.get(str(key), 0), count)
