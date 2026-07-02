"""HiCache target config 推荐配置生成。"""

from __future__ import annotations

from typing import Any

from .capacity_values import parse_int_or_none, unique_int_values, unique_policy_values


def recommend_hicache_target_config(
    capacity_unique_values: dict[str, Any],
    oracle_final_counts: dict[str, int],
    oracle_observed_max_counts: dict[str, int],
    target_config: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """从 target oracle 事实推荐 C++ HiCache target config。"""

    target_config = target_config or {}
    page_size_values = unique_int_values(capacity_unique_values, ["page_size"])
    write_policy_values = unique_policy_values(capacity_unique_values, ["write_policy"])
    prefetch_policy_values = unique_policy_values(capacity_unique_values, ["prefetch_policy"])
    prefetch_threshold_values = unique_int_values(
        capacity_unique_values, ["prefetch_threshold_pages", "thresholds.prefetch_threshold_pages"]
    )
    prefetch_capacity_limit_values = unique_int_values(
        capacity_unique_values,
        ["prefetch_capacity_limit_pages", "thresholds.prefetch_capacity_limit_pages"],
    )
    l1_pool_values = unique_int_values(capacity_unique_values, ["l1_capacity_pages", "l1_pool.capacity_pages"])
    l2_pool_values = unique_int_values(capacity_unique_values, ["l2_capacity_pages", "l2_pool.capacity_pages"])

    warnings: list[str] = []
    result: dict[str, Any] = {"enabled": True}
    evidence: dict[str, Any] = {}

    recommend_single_value(result, evidence, warnings, "page_size", page_size_values)
    recommend_single_value(result, evidence, warnings, "write_policy", write_policy_values)
    recommend_single_value(result, evidence, warnings, "prefetch_policy", prefetch_policy_values)
    recommend_single_value(result, evidence, warnings, "prefetch_threshold_pages", prefetch_threshold_values)
    recommend_single_value(result, evidence, warnings, "prefetch_capacity_limit_pages", prefetch_capacity_limit_values)
    recommend_capacity_value(
        result,
        evidence,
        warnings,
        "l1_capacity_pages",
        l1_pool_values,
        oracle_observed_max_counts.get("l1_resident_pages"),
        oracle_final_counts.get("l1_resident_pages"),
        parse_int_or_none(target_config.get("l1_capacity_pages", target_config.get("l1_capacity"))),
    )
    recommend_capacity_value(
        result,
        evidence,
        warnings,
        "l2_capacity_pages",
        l2_pool_values,
        oracle_observed_max_counts.get("l2_resident_pages"),
        oracle_final_counts.get("l2_resident_pages"),
        parse_int_or_none(target_config.get("l2_capacity_pages", target_config.get("l2_capacity"))),
    )

    required = ("page_size", "write_policy", "prefetch_policy")
    return {
        "ready": all(key in result for key in required),
        "hicache": result,
        "evidence": evidence,
        "warnings": warnings,
        "note": "Recommended config is derived from target oracle facts. Capacity is only copied when it was explicitly configured; observed occupancy peaks are reported as evidence but are not treated as capacity.",
    }


def recommend_single_value(
    result: dict[str, Any],
    evidence: dict[str, Any],
    warnings: list[str],
    field: str,
    values: list[Any],
) -> None:
    """从唯一观测值生成推荐配置字段；多值或缺失时只写 warning。"""

    if len(values) == 1:
        result[field] = values[0]
        evidence[field] = {"source": "unique_observed_value", "observed_values": values}
    elif not values:
        warnings.append(f"{field}:missing_observed_value")
        evidence[field] = {"source": "missing", "observed_values": []}
    else:
        warnings.append(f"{field}:multiple_observed_values")
        evidence[field] = {"source": "ambiguous", "observed_values": values}


def recommend_capacity_value(
    result: dict[str, Any],
    evidence: dict[str, Any],
    warnings: list[str],
    field: str,
    pool_values: list[int],
    observed_max_count: int | None,
    final_count: int | None,
    explicit_target_value: int | None = None,
) -> None:
    """推荐容量字段；容量只从显式 target config 复制。"""

    raw_pool = pool_values[0] if len(pool_values) == 1 else None
    if len(pool_values) > 1:
        warnings.append(f"{field}:multiple_observed_pool_values")
    if explicit_target_value is None or explicit_target_value <= 0:
        warnings.append(f"{field}:not_auto_recommended")
        evidence[field] = {
            "source": "not_auto_recommended",
            "observed_pool_values": pool_values,
            "observed_max_count": observed_max_count,
            "final_count": final_count,
            "reason": "Runtime occupancy and pool snapshots are diagnostic evidence, not a stable capacity parameter for target prediction.",
        }
        return
    selected = explicit_target_value
    if final_count is not None and selected < final_count:
        warnings.append(f"{field}:selected_below_final_count")
    if raw_pool is not None and observed_max_count is not None and observed_max_count > raw_pool:
        warnings.append(f"{field}:observed_max_exceeds_pool")
    result[field] = selected
    evidence[field] = {
        "source": "explicit_target_config",
        "observed_pool_values": pool_values,
        "observed_max_count": observed_max_count,
        "final_count": final_count,
        "selected": selected,
    }
