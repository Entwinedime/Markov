"""HiCache capacity and target-config oracle audit helpers."""

from __future__ import annotations

import json
from typing import Any

from .oracle_state import (
    derived_hicache_state_from_snapshot,
    snapshot_is_completed_state,
    snapshot_is_hiradix_cache_state,
    snapshot_object_id_prefix,
    snapshot_timeline_sort_key,
    union_hicache_states,
)


def extract_hicache_capacity_oracle_state(snapshots: list[dict[str, Any]]) -> dict[str, Any]:
    """汇总 oracle trace 中的 capacity/policy 快照。

    这部分只作为验证解释输出，不参与 state diff。它的用途是把真实运行中
    暴露的 L1/L2 pool 容量、可用量和 policy 参数沉淀出来，后续用于减少
    跨配置 prediction 对手工 capacity 配置的依赖。
    """

    object_id_prefix_counts: dict[str, int] = {}
    unique_values: dict[str, set[str]] = {}
    samples: list[dict[str, Any]] = []
    snapshot_count = 0
    for row in snapshots:
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
            continue
        capacity = snapshot.get("capacity")
        if not isinstance(capacity, dict):
            continue
        snapshot_count += 1
        object_id_prefix = snapshot_object_id_prefix(row, snapshot)
        object_id_prefix_counts[object_id_prefix] = object_id_prefix_counts.get(object_id_prefix, 0) + 1
        for key, value in flatten_hicache_capacity_scalars(capacity):
            unique_values.setdefault(key, set()).add(json.dumps(value, ensure_ascii=False, sort_keys=True))
        if len(samples) < 5:
            samples.append(
                {
                    "object_id_prefix": object_id_prefix,
                    "page_size": capacity.get("page_size"),
                    "write_policy": capacity.get("write_policy"),
                    "prefetch_policy": capacity.get("prefetch_policy"),
                    "l1_capacity_pages": capacity.get("l1_capacity_pages"),
                    "l1_available_pages": capacity.get("l1_available_pages"),
                    "l2_capacity_pages": capacity.get("l2_capacity_pages"),
                    "l2_available_pages": capacity.get("l2_available_pages"),
                    "prefetch_threshold_pages": capacity.get("prefetch_threshold_pages"),
                    "prefetch_capacity_limit_pages": capacity.get("prefetch_capacity_limit_pages"),
                }
            )

    return {
        "ready": snapshot_count > 0,
        "snapshot_count": snapshot_count,
        "object_id_prefix_counts": dict(sorted(object_id_prefix_counts.items())),
        "unique_values": {
            key: [json.loads(value) for value in sorted(values)]
            for key, values in sorted(unique_values.items())
        },
        "samples": samples,
    }


def build_hicache_capacity_config_audit(
    capacity_oracle: dict[str, Any],
    target_config: dict[str, Any],
    oracle_final_counts: dict[str, int],
    oracle_observed_max_counts: dict[str, int] | None = None,
) -> dict[str, Any]:
    """检查 C++ target config 与真实 capacity/policy 事实的一致性。

    该结果只做诊断，不直接决定 validation_ready。raw pool capacity 可能大于
    HiCache 对应 tier 的有效可用 budget，因此“低于 observed pool capacity”
    是需要解释的提示，不一定是错误。
    """

    unique_values = capacity_oracle.get("unique_values") if isinstance(capacity_oracle.get("unique_values"), dict) else {}
    observed_max_counts = oracle_observed_max_counts or {}
    target = {
        "page_size": optional_int(target_config.get("page_size")),
        "l1_capacity_pages": optional_int(target_config.get("l1_capacity_pages", target_config.get("l1_capacity"))),
        "l2_capacity_pages": optional_int(target_config.get("l2_capacity_pages", target_config.get("l2_capacity"))),
        "write_policy": normalize_policy_value(target_config.get("write_policy")),
        "prefetch_policy": normalize_policy_value(target_config.get("prefetch_policy")),
        "prefetch_threshold_pages": optional_int(target_config.get("prefetch_threshold_pages")),
        "prefetch_capacity_limit_pages": optional_int(target_config.get("prefetch_capacity_limit_pages")),
    }
    comparisons = {
        "page_size": _compare_int_config("page_size", target["page_size"], _unique_int_values(unique_values, ["page_size"])),
        "write_policy": _compare_policy_config("write_policy", target["write_policy"], _unique_policy_values(unique_values, ["write_policy"])),
        "prefetch_policy": _compare_policy_config("prefetch_policy", target["prefetch_policy"], _unique_policy_values(unique_values, ["prefetch_policy"])),
        "prefetch_threshold_pages": _compare_int_config(
            "prefetch_threshold_pages",
            target["prefetch_threshold_pages"],
            _unique_int_values(unique_values, ["prefetch_threshold_pages", "thresholds.prefetch_threshold_pages"]),
        ),
        "prefetch_capacity_limit_pages": _compare_int_config(
            "prefetch_capacity_limit_pages",
            target["prefetch_capacity_limit_pages"],
            _unique_int_values(unique_values, ["prefetch_capacity_limit_pages", "thresholds.prefetch_capacity_limit_pages"]),
        ),
        "l1_capacity_pages": _compare_capacity_config(
            "l1_capacity_pages",
            target["l1_capacity_pages"],
            _unique_int_values(unique_values, ["l1_capacity_pages", "l1_pool.capacity_pages"]),
            oracle_final_counts.get("l1_resident_pages"),
            observed_max_counts.get("l1_resident_pages"),
        ),
        "l2_capacity_pages": _compare_capacity_config(
            "l2_capacity_pages",
            target["l2_capacity_pages"],
            _unique_int_values(unique_values, ["l2_capacity_pages", "l2_pool.capacity_pages"]),
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


def recommend_hicache_target_config(
    capacity_unique_values: dict[str, Any],
    oracle_final_counts: dict[str, int],
    oracle_observed_max_counts: dict[str, int],
    target_config: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """从 target oracle 事实推荐 C++ HiCache target config。

    推荐值只用于修复指导，不在本次 runner 中反向覆盖 C++ 输入。page size
    和 policy 是稳定配置事实，可以自动写入推荐配置；capacity 不从 final
    count 或 observed max 自动反推，因为它们描述的是运行时占用，不等价于
    SGLang 的有效容量。只有调用方已经显式提供 target capacity 时才保留。
    """

    target_config = target_config or {}
    page_size_values = _unique_int_values(capacity_unique_values, ["page_size"])
    write_policy_values = _unique_policy_values(capacity_unique_values, ["write_policy"])
    prefetch_policy_values = _unique_policy_values(capacity_unique_values, ["prefetch_policy"])
    prefetch_threshold_values = _unique_int_values(capacity_unique_values, ["prefetch_threshold_pages", "thresholds.prefetch_threshold_pages"])
    prefetch_capacity_limit_values = _unique_int_values(
        capacity_unique_values,
        ["prefetch_capacity_limit_pages", "thresholds.prefetch_capacity_limit_pages"],
    )
    l1_pool_values = _unique_int_values(capacity_unique_values, ["l1_capacity_pages", "l1_pool.capacity_pages"])
    l2_pool_values = _unique_int_values(capacity_unique_values, ["l2_capacity_pages", "l2_pool.capacity_pages"])

    warnings: list[str] = []
    result: dict[str, Any] = {"enabled": True}
    evidence: dict[str, Any] = {}

    _recommend_single_value(result, evidence, warnings, "page_size", page_size_values)
    _recommend_single_value(result, evidence, warnings, "write_policy", write_policy_values)
    _recommend_single_value(result, evidence, warnings, "prefetch_policy", prefetch_policy_values)
    _recommend_single_value(result, evidence, warnings, "prefetch_threshold_pages", prefetch_threshold_values)
    _recommend_single_value(result, evidence, warnings, "prefetch_capacity_limit_pages", prefetch_capacity_limit_values)
    _recommend_capacity_value(
        result,
        evidence,
        warnings,
        "l1_capacity_pages",
        l1_pool_values,
        oracle_observed_max_counts.get("l1_resident_pages"),
        oracle_final_counts.get("l1_resident_pages"),
        optional_int(target_config.get("l1_capacity_pages", target_config.get("l1_capacity"))),
    )
    _recommend_capacity_value(
        result,
        evidence,
        warnings,
        "l2_capacity_pages",
        l2_pool_values,
        oracle_observed_max_counts.get("l2_resident_pages"),
        oracle_final_counts.get("l2_resident_pages"),
        optional_int(target_config.get("l2_capacity_pages", target_config.get("l2_capacity"))),
    )

    required = ("page_size", "write_policy", "prefetch_policy")
    return {
        "ready": all(key in result for key in required),
        "hicache": result,
        "evidence": evidence,
        "warnings": warnings,
        "note": "Recommended config is derived from target oracle facts. Capacity is only copied when it was explicitly configured; observed occupancy peaks are reported as evidence but are not treated as capacity.",
    }


def _recommend_single_value(
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


def _recommend_capacity_value(
    result: dict[str, Any],
    evidence: dict[str, Any],
    warnings: list[str],
    field: str,
    pool_values: list[int],
    observed_max_count: int | None,
    final_count: int | None,
    explicit_target_value: int | None = None,
) -> None:
    """推荐容量字段。

    容量只从显式 target config 复制；runtime occupancy 和 pool snapshot 仅作为诊断证据。
    """

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
    source = "explicit_target_config"
    if final_count is not None and selected < final_count:
        warnings.append(f"{field}:selected_below_final_count")
    if raw_pool is not None and observed_max_count is not None and observed_max_count > raw_pool:
        warnings.append(f"{field}:observed_max_exceeds_pool")
    result[field] = selected
    evidence[field] = {
        "source": source,
        "observed_pool_values": pool_values,
        "observed_max_count": observed_max_count,
        "final_count": final_count,
        "selected": selected,
    }


def _compare_int_config(field: str, target_value: int | None, observed_values: list[int]) -> dict[str, Any]:
    """比较整数型 target config 和 oracle 观测值。"""

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
    """比较 policy 型 target config 和 oracle 观测值。"""

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
    """比较容量 config、oracle final count 和 oracle observed max count。"""

    if target_value is None or target_value <= 0:
        status = "not_configured"
    elif not observed_values:
        status = "no_observed_value"
    elif target_value in observed_values:
        status = "matches_observed_pool"
    elif target_value > max(observed_values):
        status = "target_exceeds_observed_pool"
    else:
        status = "target_below_observed_pool"

    if oracle_final_count is None:
        final_status = "no_oracle_final_count"
    elif target_value is None or target_value <= 0:
        final_status = "not_configured"
    elif target_value < oracle_final_count:
        final_status = "target_below_oracle_final_count"
    elif target_value == oracle_final_count:
        final_status = "matches_oracle_final_count"
    else:
        final_status = "target_above_oracle_final_count"

    if oracle_observed_max_count is None:
        observed_max_status = "no_oracle_observed_max_count"
    elif target_value is None or target_value <= 0:
        observed_max_status = "not_configured"
    elif target_value < oracle_observed_max_count:
        observed_max_status = "target_below_oracle_observed_max_count"
    elif target_value == oracle_observed_max_count:
        observed_max_status = "matches_oracle_observed_max_count"
    else:
        observed_max_status = "target_above_oracle_observed_max_count"

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


def _unique_int_values(unique_values: dict[str, Any], keys: list[str]) -> list[int]:
    """从 capacity oracle unique_values 中收集整数值。"""

    values: set[int] = set()
    for key in keys:
        raw_values = unique_values.get(key)
        if not isinstance(raw_values, list):
            continue
        for value in raw_values:
            item = optional_int(value)
            if item is not None:
                values.add(item)
    return sorted(values)


def _unique_policy_values(unique_values: dict[str, Any], keys: list[str]) -> list[str]:
    """从 capacity oracle unique_values 中收集规整后的 policy 值。"""

    values: set[str] = set()
    for key in keys:
        raw_values = unique_values.get(key)
        if not isinstance(raw_values, list):
            continue
        for value in raw_values:
            normalized = normalize_policy_value(value)
            if normalized:
                values.add(normalized)
    return sorted(values)


def optional_int(value: Any) -> int | None:
    """严格解析正整数候选；bool 和非法值返回 None。"""

    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def normalize_policy_value(value: Any) -> str:
    """规整 policy 字符串，使用下划线形式。"""

    if value is None:
        return ""
    return str(value).strip().lower().replace("-", "_")


def flatten_hicache_capacity_scalars(value: Any, prefix: str = "") -> list[tuple[str, Any]]:
    """把 capacity snapshot 中的嵌套标量展开成点分路径。"""

    rows: list[tuple[str, Any]] = []
    if isinstance(value, dict):
        for key, item in sorted(value.items()):
            child_prefix = f"{prefix}.{key}" if prefix else str(key)
            rows.extend(flatten_hicache_capacity_scalars(item, child_prefix))
        return rows
    if value is None or isinstance(value, (list, tuple, set)):
        return rows
    if isinstance(value, (str, int, float, bool)):
        rows.append((prefix, value))
    return rows


def observed_max_derived_state_counts(snapshots: list[dict[str, Any]]) -> dict[str, int]:
    """计算 raw snapshot 时间线上每个状态集合达到过的最大规模。

    final state 只描述 run 结束时的状态，不能代表容量压力峰值。这里按
    HiRadixCache object 时间线更新多进程 state union，再统计峰值，用于
    capacity config audit 判断 target budget 是否低于真实曾经达到过的 resident set。
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
    """用一个 state 更新每个集合字段达到过的最大规模。"""

    for key, value in state.items():
        if not isinstance(value, list):
            continue
        count = len({str(item) for item in value if item is not None})
        max_counts[str(key)] = max(max_counts.get(str(key), 0), count)
