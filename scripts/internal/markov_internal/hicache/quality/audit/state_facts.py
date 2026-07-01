"""HiCache 专属的 profiling quality 规则。"""

from __future__ import annotations

import json
from collections import Counter, defaultdict
from typing import Any

from ...core.facts import (
    HICACHE_CONSUMER_INPUT_CONTRACT,
    HICACHE_CONSUMER_STATE_MODEL,
    parse_fact,
    parse_fact_or_none,
)
from ...core.tokens import token_dictionary_issues


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


STATE_FACT_REQUIRED_FIELDS_BY_ROLE = {
    "cache_lookup_input": (
        "request_id",
        "cache_scope",
        "seq_no",
        "source_page_size",
        "token_dictionary",
        "full_path_span",
        "token_count",
    ),
    "cache_lifecycle_commit": (
        "request_id",
        "cache_scope",
        "seq_no",
        "lifecycle_kind",
        "source_page_size",
        "token_dictionary",
        "full_path_span",
        "token_count",
    ),
    "cache_extend_input": (
        "cache_scope",
        "seq_no",
        "source_page_size",
        "batch_kind",
        "request_ids",
        "request_positions",
        "batch_size",
        "token_dictionaries",
        "full_path_spans",
        "token_counts",
    ),
    "prefetch_candidate_anchor": (
        "request_id",
        "cache_scope",
        "seq_no",
        "source_page_size",
        "token_dictionary",
        "full_path_span",
        "token_count",
    ),
}

STATE_FACT_DICTIONARY_FIELDS_BY_ROLE = {
    "cache_lookup_input": ("token_dictionary",),
    "cache_lifecycle_commit": ("token_dictionary",),
    "cache_extend_input": ("token_dictionaries",),
    "prefetch_candidate_anchor": ("token_dictionary",),
}

STATE_FACT_SPAN_FIELDS_BY_ROLE = {
    "cache_lookup_input": ("full_path_span",),
    "cache_lifecycle_commit": ("full_path_span",),
    "cache_extend_input": ("full_path_spans",),
    "prefetch_candidate_anchor": ("full_path_span",),
}

REQUIRED_COMPLETED_STATE_FACT_ROLES = ()


def observe_mechanism(counter: Counter[str], args: dict[str, Any]) -> None:
    """把完成态事件角色映射为 workload 机制命中。"""

    fact = parse_fact_or_none(args)
    if fact is None:
        return
    if not _is_completed_fact_role(args, fact.role):
        return
    mechanism = ROLE_TO_MECHANISM.get(fact.role)
    if mechanism:
        counter[mechanism] += 1


def configured_mechanisms(configured_targets: dict[str, dict[str, Any]]) -> list[str]:
    """根据配置 target 的 fact.role 推导理论可观测机制。"""

    mechanisms: set[str] = set()
    for target in configured_targets.values():
        role = configured_fact_role(target)
        mechanism = ROLE_TO_MECHANISM.get(role)
        if mechanism:
            mechanisms.add(mechanism)
    return sorted(mechanisms)


def configured_fact_role(target: dict[str, Any]) -> str:
    """读取 target 配置中的 fact.role。"""

    fact = target.get("fact")
    if isinstance(fact, dict):
        role = fact.get("role")
        if isinstance(role, str):
            return role
    return ""


def new_hicache_state_fact_accumulator() -> dict[str, Any]:
    """创建 HiCache state fact 覆盖率累加器。"""

    return {
        "counts": Counter(),
        "class_events": Counter(),
        "role_events": Counter(),
        "consumer_events": Counter(),
        "role_completed_events": Counter(),
        "missing_fields": Counter(),
        "missing_fields_by_role": defaultdict(Counter),
        "invalid_token_dictionary_issues": Counter(),
        "invalid_token_dictionary_issues_by_role": defaultdict(Counter),
        "invalid_token_dictionary_samples": [],
        "dictionary_ids": set(),
        "dictionary_ids_with_tokens": set(),
        "span_path_ids": set(),
        "seq_by_scope": defaultdict(list),
    }


def observe_hicache_state_fact(
    accumulator: dict[str, Any],
    args: dict[str, Any],
) -> None:
    """检查单个事件是否满足 HiCache state fact 合同。"""

    if not is_hicache_profile_event(args):
        return
    fact = parse_fact_or_none(args)
    if fact is None:
        return
    accumulator["class_events"][fact.fact_class] += 1
    accumulator["role_events"][fact.role] += 1
    for consumer in fact.consumers:
        accumulator["consumer_events"][consumer] += 1
    if _is_completed_state_model_fact(args):
        _observe_token_references(accumulator, args)
    if not fact.has_consumer(HICACHE_CONSUMER_STATE_MODEL):
        return
    if fact.fact_class != "workload_identity":
        accumulator["counts"]["state_model_consumer_on_non_state_fact"] += 1
        return
    accumulator["counts"]["state_model_events"] += 1
    if fact.role not in STATE_FACT_REQUIRED_FIELDS_BY_ROLE:
        accumulator["counts"]["unknown_state_model_role_events"] += 1
        if _is_completed_fact_role(args, fact.role):
            accumulator["counts"]["missing_required_fact_events"] += 1
            accumulator["missing_fields"]["fact.role"] += 1
        return
    if not _is_completed_fact_role(args, fact.role):
        return

    accumulator["counts"]["required_events"] += 1
    accumulator["role_completed_events"][fact.role] += 1
    missing = _missing_state_fact_fields(args, fact.role)
    missing.extend(_batch_state_fact_errors(args, fact.role))
    if missing:
        accumulator["counts"]["missing_required_fact_events"] += 1
        for field in missing:
            accumulator["missing_fields"][field] += 1
            accumulator["missing_fields_by_role"][fact.role][field] += 1

    scope = args.get("cache_scope")
    seq_no = _int_or_none(args.get("seq_no"))
    if _has_fact(scope) and seq_no is not None:
        accumulator["seq_by_scope"][str(scope)].append(seq_no)


def _observe_token_references(accumulator: dict[str, Any], args: dict[str, Any]) -> None:
    """记录 state model 可消费的 token dictionary/span 引用。"""

    fact = parse_fact(args)
    role = fact.role
    for field in STATE_FACT_DICTIONARY_FIELDS_BY_ROLE.get(role, ()):
        value = args.get(field)
        for item in _fact_items(value):
            if not isinstance(item, dict):
                continue
            token_path_id = item.get("token_path_id")
            if isinstance(token_path_id, str) and token_path_id:
                accumulator["dictionary_ids"].add(token_path_id)
                if isinstance(item.get("token_ids"), list):
                    accumulator["dictionary_ids_with_tokens"].add(token_path_id)
                    for issue in token_dictionary_issues(item):
                        issue_name = str(issue.get("issue") or "token_dictionary_invalid")
                        accumulator["invalid_token_dictionary_issues"][issue_name] += 1
                        accumulator["invalid_token_dictionary_issues_by_role"][role][issue_name] += 1
                        if len(accumulator["invalid_token_dictionary_samples"]) < 8:
                            accumulator["invalid_token_dictionary_samples"].append(
                                {
                                    "role": role,
                                    "field": field,
                                    **issue,
                                }
                            )
    for field in STATE_FACT_SPAN_FIELDS_BY_ROLE.get(role, ()):
        value = args.get(field)
        for item in _fact_items(value):
            if not isinstance(item, dict):
                continue
            path_id = item.get("path_id")
            if isinstance(path_id, str) and path_id:
                accumulator["span_path_ids"].add(path_id)


def _missing_state_fact_fields(args: dict[str, Any], role: str) -> list[str]:
    """返回某个 state fact role 缺失的必需字段列表。"""

    missing = [field for field in STATE_FACT_REQUIRED_FIELDS_BY_ROLE.get(role, ()) if not _has_fact(args.get(field))]
    for field in STATE_FACT_DICTIONARY_FIELDS_BY_ROLE.get(role, ()):
        value = args.get(field)
        if value is None:
            continue
        items = _fact_items(value)
        if not items or any(not _has_token_dictionary(item) for item in items):
            missing.append(f"{field}.token_path_id")
    for field in STATE_FACT_SPAN_FIELDS_BY_ROLE.get(role, ()):
        value = args.get(field)
        if value is None:
            continue
        items = _fact_items(value)
        if not items or any(not _has_token_span(item) for item in items):
            missing.append(f"{field}.path_id")
    return missing


def finalize_hicache_state_facts(accumulator: dict[str, Any]) -> dict[str, Any]:
    """汇总 HiCache state fact 合同检查结果。"""

    counts: Counter[str] = accumulator["counts"]
    missing_token_dictionary_refs = sorted(accumulator["span_path_ids"] - accumulator["dictionary_ids"])
    dictionary_ids_without_tokens = sorted(accumulator["dictionary_ids"] - accumulator["dictionary_ids_with_tokens"])
    invalid_token_dictionary_issue_count = sum(accumulator["invalid_token_dictionary_issues"].values())
    route_error_events = counts["state_model_consumer_on_non_state_fact"] + counts["unknown_state_model_role_events"]
    missing_required_roles = [
        role for role in REQUIRED_COMPLETED_STATE_FACT_ROLES if accumulator["role_completed_events"].get(role, 0) <= 0
    ]
    seq_order_error_count = 0
    for seq_values in accumulator["seq_by_scope"].values():
        previous = None
        for value in seq_values:
            if previous is not None and value <= previous:
                seq_order_error_count += 1
            previous = value
    return {
        "class_events": dict(sorted(accumulator["class_events"].items())),
        "role_events": dict(sorted(accumulator["role_events"].items())),
        "consumer_events": dict(sorted(accumulator["consumer_events"].items())),
        "workload_identity_event_count": accumulator["class_events"]["workload_identity"],
        "hicache_state_model_event_count": accumulator["consumer_events"][HICACHE_CONSUMER_STATE_MODEL],
        "input_contract_event_count": accumulator["consumer_events"][HICACHE_CONSUMER_INPUT_CONTRACT],
        "required_events": counts["required_events"],
        "role_completed_events": dict(sorted(accumulator["role_completed_events"].items())),
        "missing_required_roles": missing_required_roles,
        "missing_required_role_count": len(missing_required_roles),
        "missing_required_fact_events": counts["missing_required_fact_events"],
        "missing_fields": dict(sorted(accumulator["missing_fields"].items())),
        "missing_fields_by_role": {
            role: dict(sorted(counter.items()))
            for role, counter in sorted(accumulator["missing_fields_by_role"].items())
        },
        "route_error_events": route_error_events,
        "state_model_consumer_on_non_state_fact": counts["state_model_consumer_on_non_state_fact"],
        "unknown_state_model_role_events": counts["unknown_state_model_role_events"],
        "token_dictionary_paths": len(accumulator["dictionary_ids"]),
        "token_dictionary_paths_with_token_ids": len(accumulator["dictionary_ids_with_tokens"]),
        "token_span_refs": len(accumulator["span_path_ids"]),
        "missing_token_dictionary_refs": missing_token_dictionary_refs,
        "dictionary_ids_without_tokens": dictionary_ids_without_tokens,
        "invalid_token_dictionary_issue_count": invalid_token_dictionary_issue_count,
        "invalid_token_dictionary_issues": dict(sorted(accumulator["invalid_token_dictionary_issues"].items())),
        "invalid_token_dictionary_issues_by_role": {
            role: dict(sorted(counter.items()))
            for role, counter in sorted(accumulator["invalid_token_dictionary_issues_by_role"].items())
        },
        "invalid_token_dictionary_samples": accumulator["invalid_token_dictionary_samples"],
        "seq_scope_count": len(accumulator["seq_by_scope"]),
        "seq_order_error_count": seq_order_error_count,
        "ready": counts["missing_required_fact_events"] == 0
        and not missing_required_roles
        and route_error_events == 0
        and not missing_token_dictionary_refs
        and not dictionary_ids_without_tokens
        and invalid_token_dictionary_issue_count == 0
        and seq_order_error_count == 0,
    }


def _is_completed_state_model_fact(args: dict[str, Any]) -> bool:
    """判断事件是否是 state model 可消费的 completed fact。"""

    fact = parse_fact_or_none(args)
    return bool(
        fact is not None
        and _is_completed_fact_role(args, fact.role)
        and fact.has_consumer(HICACHE_CONSUMER_STATE_MODEL)
        and fact.fact_class == "workload_identity"
    )


def _is_completed_fact_role(args: dict[str, Any], role: str) -> bool:
    """判断当前 role 在合同中是否处于可消费完成态。"""

    phase = str(args.get("phase") or "").lower()
    if role == "cache_extend_input":
        return phase == "start"
    return phase == "end"


def _batch_state_fact_errors(args: dict[str, Any], role: str) -> list[str]:
    """检查 batch-level cache extend fact 的数组合同。"""

    if role != "cache_extend_input":
        return []
    errors: list[str] = []
    if str(args.get("batch_kind") or "") != "extend":
        errors.append("batch_kind.extend")
    request_ids = args.get("request_ids")
    request_positions = args.get("request_positions")
    token_dictionaries = args.get("token_dictionaries")
    full_path_spans = args.get("full_path_spans")
    token_counts = args.get("token_counts")
    if not isinstance(request_ids, list) or not request_ids:
        errors.append("request_ids.non_empty")
        request_ids = []
    expected = len(request_ids)
    for field_name, value in (
        ("request_positions", request_positions),
        ("token_dictionaries", token_dictionaries),
        ("full_path_spans", full_path_spans),
        ("token_counts", token_counts),
    ):
        if not isinstance(value, list):
            errors.append(f"{field_name}.array")
        elif len(value) != expected:
            errors.append(f"{field_name}.length")
    batch_size = _int_or_none(args.get("batch_size"))
    if batch_size is None or batch_size != expected:
        errors.append("batch_size.request_ids_length")
    string_ids = [str(item) for item in request_ids if item is not None]
    if len(string_ids) != expected or any(not item for item in string_ids):
        errors.append("request_ids.valid")
    if len(set(string_ids)) != len(string_ids):
        errors.append("request_ids.unique")
    if isinstance(request_positions, list) and len(request_positions) == expected:
        indexes: list[int] = []
        for row in request_positions:
            if not isinstance(row, dict):
                errors.append("request_positions.item")
                continue
            index = _int_or_none(row.get("index"))
            if index is None:
                errors.append("request_positions.index")
                continue
            indexes.append(index)
            row_request_id = str(row.get("request_id") or "")
            if 0 <= index < expected and row_request_id and row_request_id != string_ids[index]:
                errors.append("request_positions.request_id")
        if sorted(indexes) != list(range(expected)):
            errors.append("request_positions.coverage")
    return errors


def _fact_items(value: Any) -> list[Any]:
    """把 scalar field 和数组 field 统一成可遍历项。"""

    if isinstance(value, list):
        return value
    if value is None:
        return []
    return [value]


def _has_fact(value: Any) -> bool:
    """判断字段值是否能作为有效事实参与合同检查。"""

    if value is None:
        return False
    if isinstance(value, str):
        return bool(value)
    if isinstance(value, (list, tuple, set, dict)):
        return len(value) > 0
    return True


def _has_token_dictionary(value: Any) -> bool:
    """判断 token dictionary 是否包含模型所需的身份字段。"""

    if not isinstance(value, dict):
        return False
    return (
        isinstance(value.get("token_path_id"), str)
        and bool(value.get("token_path_id"))
        and _has_fact(value.get("token_count"))
        and _has_fact(value.get("hash_algo"))
    )


def _has_token_span(value: Any) -> bool:
    """判断 token span 是否能引用已记录 token dictionary。"""

    if not isinstance(value, dict):
        return False
    return (
        isinstance(value.get("path_id"), str)
        and bool(value.get("path_id"))
        and _has_fact(value.get("begin"))
        and _has_fact(value.get("end"))
        and _has_fact(value.get("token_count"))
        and _has_fact(value.get("hash_algo"))
    )


def _int_or_none(value: Any) -> int | None:
    """宽松解析整数，避免 bool 被误当成 0/1。"""

    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def is_hicache_profile_event(args: dict[str, Any]) -> bool:
    """识别需要参与 HiCache 专项质量审计的事件。"""

    target_id = str(args.get("target_id") or "").lower()
    if target_id.startswith(("hiradix.", "hicache.", "hicache_controller.")):
        return True
    fact = parse_fact_or_none(args)
    return fact is not None and (fact.role in ROLE_TO_MECHANISM or fact.role in STATE_FACT_REQUIRED_FIELDS_BY_ROLE)


def new_hicache_capacity_accumulator() -> dict[str, Any]:
    """创建 validation-only capacity snapshot 累加器。"""

    return {
        "snapshot_count": 0,
        "object_id_prefix_counts": Counter(),
        "unique_values": defaultdict(set),
        "samples": [],
    }


def observe_hicache_capacity(accumulator: dict[str, Any], args: dict[str, Any]) -> None:
    """从 validation-only state snapshot 中汇总 capacity/policy 证据。"""

    fact = parse_fact_or_none(args)
    if fact is None or fact.fact_class != "oracle_state" or fact.role != "state_snapshot":
        return
    snapshot = args.get("state_snapshot")
    if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
        return
    capacity = snapshot.get("capacity")
    if not isinstance(capacity, dict):
        return
    accumulator["snapshot_count"] += 1
    object_id = str(snapshot.get("object_id") or "unknown")
    object_id_prefix = object_id.split(":", 1)[0] if object_id else "unknown"
    accumulator["object_id_prefix_counts"][object_id_prefix] += 1
    for key, value in _flatten_capacity_scalars(capacity):
        accumulator["unique_values"][key].add(json.dumps(value, ensure_ascii=False, sort_keys=True))
    if len(accumulator["samples"]) < 5:
        accumulator["samples"].append(
            {
                "object_id_prefix": object_id_prefix,
                "page_size": capacity.get("page_size"),
                "write_policy": capacity.get("write_policy"),
                "prefetch_policy": capacity.get("prefetch_policy"),
                "l1_capacity_pages": capacity.get("l1_capacity_pages"),
                "l1_available_pages": capacity.get("l1_available_pages"),
                "l2_capacity_pages": capacity.get("l2_capacity_pages"),
                "l2_available_pages": capacity.get("l2_available_pages"),
                "prefetch_capacity_limit_pages": capacity.get("prefetch_capacity_limit_pages"),
            }
        )


def finalize_hicache_capacity(accumulator: dict[str, Any]) -> dict[str, Any]:
    """汇总 capacity/policy snapshot 中出现过的标量值。"""

    unique_values = {}
    for key, values in sorted(accumulator["unique_values"].items()):
        unique_values[key] = [json.loads(value) for value in sorted(values)]
    return {
        "ready": accumulator["snapshot_count"] > 0,
        "snapshot_count": accumulator["snapshot_count"],
        "object_id_prefix_counts": dict(sorted(accumulator["object_id_prefix_counts"].items())),
        "unique_values": unique_values,
        "samples": accumulator["samples"],
    }


def _flatten_capacity_scalars(value: Any, prefix: str = "") -> list[tuple[str, Any]]:
    """把嵌套 capacity 对象展开成可比较的标量路径。"""

    rows: list[tuple[str, Any]] = []
    if isinstance(value, dict):
        for key, item in sorted(value.items()):
            child_prefix = f"{prefix}.{key}" if prefix else str(key)
            rows.extend(_flatten_capacity_scalars(item, child_prefix))
        return rows
    if value is None or isinstance(value, (list, tuple, set)):
        return rows
    if isinstance(value, (str, int, float, bool)):
        rows.append((prefix, value))
    return rows
