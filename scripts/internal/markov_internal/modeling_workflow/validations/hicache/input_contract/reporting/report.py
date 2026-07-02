"""HiCache 输入合同 source/target 报告组装。"""

from __future__ import annotations

import collections
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..signature.extractor import extract_audit_events
from ..signature.path_contract import workload_identity_path_contract
from ..signature.types import DEFAULT_ROLES, KNOWN_WORKLOAD_IDENTITY_ROLES, AuditEvent


@dataclass(frozen=True)
class InputContractReportOptions:
    """输入合同 source/target 审计参数。"""

    source_trace: list[Path]
    target_trace: list[Path]
    source_label: str = "source"
    target_label: str = "target"
    roles: tuple[str, ...] = DEFAULT_ROLES
    sample: int = 8


def summarize_event(event: AuditEvent | None) -> dict[str, Any] | None:
    """把 AuditEvent 转成报告中可读的诊断结构。"""

    if event is None:
        return None
    return {
        "stream": event.stream,
        "ordinal": event.ordinal,
        "role": event.role,
        "target_id": event.target_id,
        "event_name": event.event_name,
        "ts": event.ts,
        "seq_no": event.seq_no,
        "request_id": event.request_id,
        "request_fingerprint": event.request_fingerprint,
        "cache_scope": event.cache_scope,
        "fields": event.fields,
    }


def first_sequence_mismatch(source: list[AuditEvent], target: list[AuditEvent]) -> dict[str, Any] | None:
    """定位 source/target 序列中第一个 signature 不一致位置。"""

    limit = min(len(source), len(target))
    for index in range(limit):
        if source[index].signature != target[index].signature:
            return {
                "index": index,
                "source": summarize_event(source[index]),
                "target": summarize_event(target[index]),
            }
    if len(source) != len(target):
        return {
            "index": limit,
            "source": summarize_event(source[limit] if limit < len(source) else None),
            "target": summarize_event(target[limit] if limit < len(target) else None),
        }
    return None


def counter_samples(counter: collections.Counter[str], sample: int) -> list[dict[str, Any]]:
    """从 signature counter 中提取报告样本。"""

    rows = []
    for signature, count in counter.most_common(sample):
        try:
            fields = json.loads(signature)
        except json.JSONDecodeError:
            fields = {"signature": signature}
        rows.append({"count": count, "fields": fields})
    return rows


def summarize_role(role: str, source: list[AuditEvent], target: list[AuditEvent], sample: int) -> dict[str, Any]:
    """汇总单个 role 的 count、sequence 和 canonical multiset 匹配结果。"""

    source_counter = collections.Counter(event.signature for event in source)
    target_counter = collections.Counter(event.signature for event in target)
    source_only = source_counter - target_counter
    target_only = target_counter - source_counter
    first_mismatch = first_sequence_mismatch(source, target)
    return {
        "role": role,
        "source_count": len(source),
        "target_count": len(target),
        "count_match": len(source) == len(target),
        "sequence_match": first_mismatch is None,
        "signature_multiset_match": not source_only and not target_only,
        "first_sequence_mismatch": first_mismatch,
        "source_only_signature_count": sum(source_only.values()),
        "target_only_signature_count": sum(target_only.values()),
        "source_only_samples": counter_samples(source_only, sample),
        "target_only_samples": counter_samples(target_only, sample),
    }


def build_report(options: InputContractReportOptions) -> dict[str, Any]:
    """构造 cross-input audit 完整报告。"""

    roles = set(options.roles or DEFAULT_ROLES)
    unknown_roles = sorted(roles - KNOWN_WORKLOAD_IDENTITY_ROLES)
    source_events, source_unknown, source_unmapped = extract_audit_events(
        options.source_trace,
        options.source_label,
        roles,
    )
    target_events, target_unknown, target_unmapped = extract_audit_events(
        options.target_trace,
        options.target_label,
        roles,
    )
    source_path_contract = workload_identity_path_contract(options.source_trace, roles, options.sample)
    target_path_contract = workload_identity_path_contract(options.target_trace, roles, options.sample)

    source_by_role: dict[str, list[AuditEvent]] = {role: [] for role in roles}
    target_by_role: dict[str, list[AuditEvent]] = {role: [] for role in roles}
    for event in source_events:
        source_by_role.setdefault(event.role, []).append(event)
    for event in target_events:
        target_by_role.setdefault(event.role, []).append(event)

    role_summaries = [
        summarize_role(role, source_by_role.get(role, []), target_by_role.get(role, []), options.sample)
        for role in sorted(roles)
    ]
    fact_mismatch_roles = [
        row["role"] for row in role_summaries if not row["count_match"] or not row["signature_multiset_match"]
    ]
    sequence_mismatch_roles = [
        row["role"]
        for row in role_summaries
        if row["count_match"] and row["signature_multiset_match"] and not row["sequence_match"]
    ]
    unknown_workload_identity_roles = {
        "source": dict(sorted(source_unknown.items())),
        "target": dict(sorted(target_unknown.items())),
    }
    unmapped_request_id_events = {
        "source": dict(sorted(source_unmapped.items())),
        "target": dict(sorted(target_unmapped.items())),
    }
    unmapped_roles = sorted(set(source_unmapped) | set(target_unmapped))
    path_contract_roles = sorted(
        set(source_path_contract["blocking_roles"]) | set(target_path_contract["blocking_roles"])
    )
    blocking_roles = sorted(set(fact_mismatch_roles) | set(unmapped_roles) | set(path_contract_roles))
    ready = (
        not blocking_roles
        and not source_unknown
        and not target_unknown
        and not unknown_roles
        and source_path_contract["ready"]
        and target_path_contract["ready"]
    )
    return {
        "schema": "trace_sim.hicache.workload_identity_cross_input_audit.v1",
        "source_label": options.source_label,
        "target_label": options.target_label,
        "source_traces": [str(path) for path in options.source_trace],
        "target_traces": [str(path) for path in options.target_trace],
        "roles": sorted(roles),
        "source_event_count": len(source_events),
        "target_event_count": len(target_events),
        "input_contract_ready": ready,
        "input_contract_ready_for_cross_state_rule_diagnosis": ready,
        "input_contract_blocking_roles": blocking_roles,
        "input_contract_fact_mismatch_roles": fact_mismatch_roles,
        "workload_identity_path_contract_blocking_roles": path_contract_roles,
        "source_workload_identity_path_contract": source_path_contract,
        "target_workload_identity_path_contract": target_path_contract,
        "non_blocking_sequence_mismatch_roles": sequence_mismatch_roles,
        "unknown_requested_roles": unknown_roles,
        "unknown_workload_identity_roles": unknown_workload_identity_roles,
        "unmapped_request_id_events": unmapped_request_id_events,
        "role_summaries": role_summaries,
        "request_id_policy": (
            "Raw request_id is run-local and is not part of the cross-config canonical fact. "
            "Request-scoped facts are compared with a request_fingerprint derived from path-bearing "
            "workload identity facts in the same run."
        ),
        "pass_condition": (
            "For every selected workload identity role, source and target streams must match by event count "
            "and canonical fact multiset after request_id normalization. Unknown workload identity roles and "
            "unmapped request-scoped facts are hard failures. Path-bearing workload identity facts must be directly "
            "consumable by the C++ token parser, including at least one token_ids dictionary for "
            "every referenced path_id. Sequence mismatch is diagnostic only."
        ),
    }
