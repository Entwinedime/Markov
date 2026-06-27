"""HiCache-specific profiling quality rules."""

from __future__ import annotations

import json
from collections import Counter, defaultdict
from typing import Any

from ..hicache.facts import (
    HICACHE_CONSUMER_INPUT_CONTRACT,
    HICACHE_CONSUMER_STATE_MODEL,
    parse_fact,
    parse_fact_or_none,
)
from ..hicache.tokens import token_dictionary_issues


ROLE_TO_MECHANISM = {
    "request_bound_match_anchor": "lookup",
    "cache_stage_match_path_observed": "lookup",
    "lookup_result_observed": "lookup",
    "request_lifecycle_anchor": "insert",
    "request_lifecycle_path_observed": "insert",
    "request_lifecycle_runtime_observed": "insert",
    "request_admission": "admission",
    "request_admission_observed": "admission",
    "insert_path": "insert",
    "insert_result_observed": "insert",
    "prefetch_decision": "prefetch_schedule",
    "prefetch_decision_observed": "prefetch_schedule",
    "prefetch_intent": "prefetch_schedule",
    "prefetch_intent_observed": "prefetch_schedule",
    "prefetch_check_point": "prefetch_progress",
    "storage_control_drain_boundary": "storage_control",
    "prefetch_progress_observed": "prefetch_progress",
    "maintenance_checkpoint": "maintenance",
    "capacity_request": "evict",
    "capacity_result_observed": "evict",
    "lock_scope_delta": "lock_ref",
    "lock_scope_result_observed": "lock_ref",
    "host_ref_delta_observed": "lock_ref",
    "all_blocks_cleared_observed": "maintenance",
    "load_back_request_observed": "load_back",
    "load_back_result_observed": "load_back",
    "load_enqueue_observed": "load_back",
    "load_start_observed": "load_back",
    "writeback_schedule_observed": "write_storage",
    "writeback_storage_schedule_observed": "write_storage",
    "write_enqueue_observed": "write_storage",
    "write_start_observed": "write_storage",
    "host_eviction_observed": "evict",
    "prefetch_terminate_requested_observed": "prefetch_progress",
    "request_abort_cleanup_observed": "prefetch_progress",
    "prefetch_loaded_tokens_observed": "prefetch_progress",
    "prefetch_rate_limit_observed": "prefetch_schedule",
    "prefetch_enqueue_observed": "prefetch_schedule",
    "storage_hit_query_observed": "prefetch_schedule",
    "prefetch_terminate_observed": "prefetch_progress",
    "host_mem_release_enqueue_observed": "prefetch_progress",
    "node_store_observed": "insert",
    "node_remove_observed": "evict",
    "radix_node_mutation_observed": "insert",
    "evictable_state_observed": "evict",
    "write_counter_delta_observed": "write_storage",
    "write_ack_checkpoint_observed": "write_storage",
    "load_ack_checkpoint_observed": "load_back",
    "prefetch_io_observed": "prefetch_transfer",
    "writeback_io_observed": "write_storage",
    "writeback_enqueue_observed": "write_storage",
}


STATE_FACT_REQUIRED_FIELDS_BY_ROLE = {
    "request_bound_match_anchor": (
        "request_id",
        "cache_scope",
        "seq_no",
        "source_page_size",
        "token_dictionary",
        "full_path_span",
        "token_count",
    ),
    "request_lifecycle_anchor": (
        "request_id",
        "cache_scope",
        "seq_no",
        "lifecycle_kind",
        "source_page_size",
        "token_dictionary",
        "full_path_span",
        "token_count",
    ),
    "request_admission": (
        "request_id",
        "cache_scope",
        "seq_no",
        "admission_kind",
        "source_page_size",
        "token_dictionary",
        "full_path_span",
        "token_count",
        "policy_params",
    ),
    "prefetch_decision": (
        "request_id",
        "cache_scope",
        "seq_no",
        "token_dictionary",
        "full_path_span",
        "token_count",
        "policy_params",
    ),
    "prefetch_check_point": (
        "request_id",
        "cache_scope",
        "seq_no",
        "check_kind",
    ),
    "storage_control_drain_boundary": (
        "cache_scope",
        "seq_no",
        "source_page_size",
        "check_kind",
    ),
}

STATE_FACT_EITHER_FIELDS_BY_ROLE = {
}

STATE_FACT_DICTIONARY_FIELDS_BY_ROLE = {
    "request_bound_match_anchor": ("token_dictionary",),
    "request_lifecycle_anchor": ("token_dictionary",),
    "request_admission": ("token_dictionary",),
    "prefetch_decision": ("token_dictionary",),
}

STATE_FACT_SPAN_FIELDS_BY_ROLE = {
    "request_bound_match_anchor": ("full_path_span",),
    "request_lifecycle_anchor": ("full_path_span",),
    "request_admission": ("full_path_span",),
    "prefetch_decision": ("full_path_span",),
}

REQUIRED_COMPLETED_STATE_FACT_ROLES = (
    "storage_control_drain_boundary",
)


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
        "lifecycle_anchor_paths": defaultdict(list),
        "lifecycle_observed_paths": defaultdict(list),
        "prefetch_actual_enqueue_events": 0,
        "prefetch_actual_path_events": 0,
        "prefetch_actual_positive_path_events": 0,
        "prefetch_intent_positive_path_events": 0,
        "prefetch_enqueue_positive_path_events": 0,
        "prefetch_decision_empty_path_events": 0,
        "prefetch_decision_nonempty_path_events": 0,
    }


def observe_hicache_state_fact(accumulator: dict[str, Any], args: dict[str, Any]) -> None:
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
    _observe_prefetch_path_contract(accumulator, args)
    if _is_completed_state_model_fact(args):
        _observe_token_references(accumulator, args)
    _observe_lifecycle_path_contract(accumulator, args)
    if not fact.has_consumer(HICACHE_CONSUMER_STATE_MODEL):
        return
    if fact.fact_class not in {"workload_identity", "target_policy_input", "runtime_model_checkpoint"}:
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
    if missing:
        accumulator["counts"]["missing_required_fact_events"] += 1
        for field in missing:
            accumulator["missing_fields"][field] += 1
            accumulator["missing_fields_by_role"][fact.role][field] += 1

    scope = args.get("cache_scope")
    seq_no = _int_or_none(args.get("seq_no"))
    if _has_fact(scope) and seq_no is not None:
        accumulator["seq_by_scope"][str(scope)].append(seq_no)


def _observe_prefetch_path_contract(accumulator: dict[str, Any], args: dict[str, Any]) -> None:
    """记录 prefetch model path 与实际 enqueue 证据是否一致。"""

    fact = parse_fact_or_none(args)
    if fact is None:
        return
    role = fact.role
    if role == "prefetch_intent_observed":
        if args.get("phase") == "end":
            _observe_prefetch_actual_path(accumulator, args, "intent")
        return
    if role == "prefetch_enqueue_observed":
        _observe_prefetch_actual_path(accumulator, args, "enqueue")
        return
    if args.get("phase") != "end":
        return
    if role != "prefetch_decision" or not _is_completed_state_model_fact(args):
        return
    token_count = _state_model_token_count(args)
    if token_count is None or token_count <= 0:
        accumulator["prefetch_decision_empty_path_events"] += 1
    else:
        accumulator["prefetch_decision_nonempty_path_events"] += 1


def _observe_prefetch_actual_path(accumulator: dict[str, Any], args: dict[str, Any], source_kind: str) -> None:
    """记录 source_actual prefetch 是否确实携带非空候选路径。"""

    token_count = _prefetch_actual_token_count(args, source_kind)
    if source_kind == "enqueue":
        accumulator["prefetch_actual_enqueue_events"] += 1
    accumulator["prefetch_actual_path_events"] += 1
    if token_count is None or token_count <= 0:
        return
    accumulator["prefetch_actual_positive_path_events"] += 1
    if source_kind == "intent":
        accumulator["prefetch_intent_positive_path_events"] += 1
    if source_kind == "enqueue":
        accumulator["prefetch_enqueue_positive_path_events"] += 1


def _prefetch_actual_token_count(args: dict[str, Any], source_kind: str) -> int | None:
    """从 source_actual prefetch 事件读取实际候选 token 数。"""

    candidates: list[int] = []
    fields = ("new_input_tokens", "token_count") if source_kind == "intent" else ("new_input_tokens", "host_tokens", "token_count")
    for field in fields:
        value = _int_or_none(args.get(field))
        if value is not None:
            candidates.append(value)
    span_name = "suffix_span" if source_kind == "intent" else ""
    span = args.get(span_name) if span_name else None
    if isinstance(span, dict):
        value = _int_or_none(span.get("token_count"))
        if value is not None:
            candidates.append(value)
    operation = args.get("operation")
    if isinstance(operation, dict):
        for field in ("token_count", "host_tokens"):
            value = _int_or_none(operation.get(field))
            if value is not None:
                candidates.append(value)
    return max(candidates) if candidates else None


def _state_model_token_count(args: dict[str, Any]) -> int | None:
    """从 state model fact 中读取可投影 path 的 token 数。"""

    span = args.get("full_path_span")
    if isinstance(span, dict):
        value = _int_or_none(span.get("token_count"))
        if value is not None:
            return value
    return _int_or_none(args.get("token_count"))


def _observe_lifecycle_path_contract(accumulator: dict[str, Any], args: dict[str, Any]) -> None:
    """记录 lifecycle workload anchor 与 source_actual path 证据。"""

    if args.get("phase") != "end":
        return
    fact = parse_fact_or_none(args)
    if fact is None:
        return
    role = fact.role
    if role not in {"request_lifecycle_anchor", "request_lifecycle_path_observed"}:
        return
    key = _lifecycle_path_key(args)
    if key is None:
        return
    signature = _lifecycle_path_signature(args)
    if role == "request_lifecycle_anchor":
        accumulator["lifecycle_anchor_paths"][key].append(signature)
    else:
        accumulator["lifecycle_observed_paths"][key].append(signature)


def _lifecycle_path_key(args: dict[str, Any]) -> tuple[str, str, str] | None:
    """生成同一次 run 内 lifecycle path 对照使用的 request key。"""

    cache_scope = args.get("cache_scope")
    request_id = args.get("request_id")
    lifecycle_kind = args.get("lifecycle_kind")
    if not (_has_fact(cache_scope) and _has_fact(request_id) and _has_fact(lifecycle_kind)):
        return None
    return (str(cache_scope), str(request_id), str(lifecycle_kind))


def _lifecycle_path_signature(args: dict[str, Any]) -> dict[str, Any]:
    """提取 lifecycle path 的可比较签名。"""

    dictionary = args.get("token_dictionary")
    span = args.get("full_path_span")
    return {
        "token_path_id": dictionary.get("token_path_id") if isinstance(dictionary, dict) else None,
        "token_count": _int_or_none(args.get("token_count")),
        "span_path_id": span.get("path_id") if isinstance(span, dict) else None,
        "span_begin": _int_or_none(span.get("begin")) if isinstance(span, dict) else None,
        "span_end": _int_or_none(span.get("end")) if isinstance(span, dict) else None,
        "span_token_count": _int_or_none(span.get("token_count")) if isinstance(span, dict) else None,
        "hash_algo": span.get("hash_algo") if isinstance(span, dict) else None,
    }


def _observe_token_references(accumulator: dict[str, Any], args: dict[str, Any]) -> None:
    """记录 state model 可消费的 token dictionary/span 引用。"""

    fact = parse_fact(args)
    role = fact.role
    for field in STATE_FACT_DICTIONARY_FIELDS_BY_ROLE.get(role, ()):
        value = args.get(field)
        if not isinstance(value, dict):
            continue
        token_path_id = value.get("token_path_id")
        if isinstance(token_path_id, str) and token_path_id:
            accumulator["dictionary_ids"].add(token_path_id)
            if isinstance(value.get("token_ids"), list):
                accumulator["dictionary_ids_with_tokens"].add(token_path_id)
                for issue in token_dictionary_issues(value):
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
        if not isinstance(value, dict):
            continue
        path_id = value.get("path_id")
        if isinstance(path_id, str) and path_id:
            accumulator["span_path_ids"].add(path_id)


def _missing_state_fact_fields(args: dict[str, Any], role: str) -> list[str]:
    """返回某个 state fact role 缺失的必需字段列表。"""

    missing = [
        field
        for field in STATE_FACT_REQUIRED_FIELDS_BY_ROLE.get(role, ())
        if not _has_fact(args.get(field))
    ]
    for choices in STATE_FACT_EITHER_FIELDS_BY_ROLE.get(role, ()):
        if not any(_has_fact(args.get(field)) for field in choices):
            missing.append("|".join(choices))
    if (
        role == "storage_control_drain_boundary"
        and _has_fact(args.get("check_kind"))
        and str(args.get("check_kind")) != "storage_control_drain"
    ):
        missing.append("check_kind.storage_control_drain")

    for field in STATE_FACT_DICTIONARY_FIELDS_BY_ROLE.get(role, ()):
        value = args.get(field)
        if value is not None and not _has_token_dictionary(value):
            missing.append(f"{field}.token_path_id")
    for field in STATE_FACT_SPAN_FIELDS_BY_ROLE.get(role, ()):
        value = args.get(field)
        if value is not None and not _has_token_span(value):
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
        role
        for role in REQUIRED_COMPLETED_STATE_FACT_ROLES
        if accumulator["role_completed_events"].get(role, 0) <= 0
    ]
    seq_order_error_count = 0
    for seq_values in accumulator["seq_by_scope"].values():
        previous = None
        for value in seq_values:
            if previous is not None and value <= previous:
                seq_order_error_count += 1
            previous = value
    lifecycle_path_contract = _finalize_lifecycle_path_contract(accumulator)
    prefetch_path_contract = _finalize_prefetch_path_contract(accumulator)
    return {
        "class_events": dict(sorted(accumulator["class_events"].items())),
        "role_events": dict(sorted(accumulator["role_events"].items())),
        "consumer_events": dict(sorted(accumulator["consumer_events"].items())),
        "workload_identity_event_count": accumulator["class_events"]["workload_identity"],
        "target_policy_input_event_count": accumulator["class_events"]["target_policy_input"],
        "runtime_model_checkpoint_event_count": accumulator["class_events"]["runtime_model_checkpoint"],
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
        **lifecycle_path_contract,
        **prefetch_path_contract,
        "ready": counts["missing_required_fact_events"] == 0
        and not missing_required_roles
        and route_error_events == 0
        and not missing_token_dictionary_refs
        and not dictionary_ids_without_tokens
        and invalid_token_dictionary_issue_count == 0
        and seq_order_error_count == 0
        and prefetch_path_contract["prefetch_path_contract_error_count"] == 0,
    }


def _finalize_prefetch_path_contract(accumulator: dict[str, Any]) -> dict[str, Any]:
    """汇总 prefetch enqueue 与 model candidate path 的合同检查。"""

    enqueue_events = int(accumulator["prefetch_actual_enqueue_events"])
    actual_path_events = int(accumulator["prefetch_actual_path_events"])
    positive_actual_path_events = int(accumulator["prefetch_actual_positive_path_events"])
    intent_positive_path_events = int(accumulator["prefetch_intent_positive_path_events"])
    enqueue_positive_path_events = int(accumulator["prefetch_enqueue_positive_path_events"])
    nonempty_path_events = int(accumulator["prefetch_decision_nonempty_path_events"])
    empty_path_events = int(accumulator["prefetch_decision_empty_path_events"])
    error_count = 1 if positive_actual_path_events > 0 and nonempty_path_events == 0 else 0
    return {
        "prefetch_actual_enqueue_events": enqueue_events,
        "prefetch_actual_path_events": actual_path_events,
        "prefetch_actual_positive_path_events": positive_actual_path_events,
        "prefetch_intent_positive_path_events": intent_positive_path_events,
        "prefetch_enqueue_positive_path_events": enqueue_positive_path_events,
        "prefetch_decision_empty_path_events": empty_path_events,
        "prefetch_decision_nonempty_path_events": nonempty_path_events,
        "prefetch_path_contract_error_count": error_count,
    }


def _finalize_lifecycle_path_contract(accumulator: dict[str, Any]) -> dict[str, Any]:
    """对照 lifecycle workload path 与 source_actual observed path。"""

    anchors: dict[tuple[str, str, str], list[dict[str, Any]]] = accumulator["lifecycle_anchor_paths"]
    observed: dict[tuple[str, str, str], list[dict[str, Any]]] = accumulator["lifecycle_observed_paths"]
    missing_observed = 0
    missing_anchor = 0
    path_disagree = 0
    token_count_disagree = 0
    for key in sorted(set(anchors) | set(observed)):
        anchor_rows = anchors.get(key, [])
        observed_rows = observed.get(key, [])
        missing_observed += max(0, len(anchor_rows) - len(observed_rows))
        missing_anchor += max(0, len(observed_rows) - len(anchor_rows))
        for anchor, row in zip(anchor_rows, observed_rows):
            if _lifecycle_token_count_signature(anchor) != _lifecycle_token_count_signature(row):
                token_count_disagree += 1
            if _lifecycle_path_identity(anchor) != _lifecycle_path_identity(row):
                path_disagree += 1
    return {
        "lifecycle_anchor_path_events": sum(len(rows) for rows in anchors.values()),
        "lifecycle_observed_path_events": sum(len(rows) for rows in observed.values()),
        "lifecycle_anchor_observed_missing": missing_observed,
        "lifecycle_observed_anchor_missing": missing_anchor,
        "lifecycle_anchor_observed_path_disagree": path_disagree,
        "lifecycle_anchor_observed_token_count_disagree": token_count_disagree,
        "lifecycle_path_contract_error_count": missing_observed + missing_anchor + path_disagree + token_count_disagree,
    }


def _lifecycle_path_identity(signature: dict[str, Any]) -> tuple[Any, ...]:
    """返回 lifecycle path identity 相关字段。"""

    return (
        signature.get("token_path_id"),
        signature.get("span_path_id"),
        signature.get("span_begin"),
        signature.get("span_end"),
        signature.get("hash_algo"),
    )


def _lifecycle_token_count_signature(signature: dict[str, Any]) -> tuple[Any, ...]:
    """返回 lifecycle token 数相关字段。"""

    return (
        signature.get("token_count"),
        signature.get("span_token_count"),
    )


def _is_completed_state_model_fact(args: dict[str, Any]) -> bool:
    """判断事件是否是 state model 可消费的 completed fact。"""

    fact = parse_fact_or_none(args)
    return bool(
        fact is not None
        and _is_completed_fact_role(args, fact.role)
        and fact.has_consumer(HICACHE_CONSUMER_STATE_MODEL)
        and fact.fact_class in {"workload_identity", "target_policy_input", "runtime_model_checkpoint"}
    )


def _is_completed_fact_role(args: dict[str, Any], role: str) -> bool:
    """判断当前 role 在合同中是否处于可消费完成态。"""

    phase = str(args.get("phase") or "").lower()
    if role in {"prefetch_check_point", "storage_control_drain_boundary"}:
        return phase == "instant"
    return phase == "end"


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
