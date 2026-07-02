"""predicted HiCache transition trace 的模型侧自检。"""

from __future__ import annotations

import collections
from pathlib import Path
from typing import Any

from ...oracle.diff.multiset import count_rows_by_transition_kind
from ..replay.record_schema import (
    ADVISORY_RECORD_FIELDS,
    CORE_RECORD_FIELDS,
    KNOWN_TRANSITION_KINDS,
    load_predicted_trace,
    missing_core_field,
    optional_int,
    predicted_final_state,
    predicted_records,
    record_pages,
)
from ..replay.engine import replay_predicted_records
from ..replay.final_state_compare import compare_replay_final_state


def build_model_self_check(predicted_trace_path: Path, *, sample_limit: int) -> dict[str, Any]:
    """构建第 1 阶段模型侧 transition 自洽报告。"""

    predicted = load_predicted_trace(predicted_trace_path)
    records = predicted_records(predicted)
    schema_check = check_predicted_trace_schema(predicted, records, sample_limit=sample_limit)
    replay = replay_predicted_records(records, sample_limit=sample_limit)
    final_state = predicted_final_state(predicted)
    replay_check = compare_replay_final_state(replay, final_state, sample_limit=sample_limit)
    provenance_check = build_provenance_check(records, sample_limit=sample_limit)
    lifecycle_check = build_lifecycle_check(records, sample_limit=sample_limit)

    ready = (
        not schema_check["unknown_transition_kinds"]
        and not schema_check["missing_required_fields"]
        and replay_check["replay_final_state_match"]
        and not replay["state_constraint_violations"]
        and not lifecycle_check["unclosed_required_lifecycle"]
    )
    return {
        "schema": "trace_sim.hicache.model_transition_self_check.v1",
        "ready": ready,
        "predicted_trace_path": str(predicted_trace_path),
        "record_count": len(records),
        "schema_check": schema_check,
        "replay_check": replay_check,
        "state_constraint_check": {
            "state_constraint_violation_count": len(replay["state_constraint_violations"]),
            "state_constraint_violations": replay["state_constraint_violations"][:sample_limit],
            "ref_balance_issue_count": len(replay["ref_balance_issues"]),
            "ref_balance_issues": replay["ref_balance_issues"][:sample_limit],
            "derived_noop_transition_count": len(replay["derived_noop_transitions"]),
            "derived_noop_transitions": replay["derived_noop_transitions"][:sample_limit],
        },
        "provenance_check": provenance_check,
        "lifecycle_check": lifecycle_check,
        "replayed_final_state": replay["final_state"],
        "replayed_delta_summary": {
            "delta_row_count": len(replay["delta_rows"]),
            "delta_count_by_kind": count_rows_by_transition_kind(replay["delta_rows"]),
        },
    }


def check_predicted_trace_schema(
    predicted: dict[str, Any], records: list[dict[str, Any]], *, sample_limit: int
) -> dict[str, Any]:
    """检查 predicted transition trace 的 schema 完整性。"""

    missing_required: list[dict[str, Any]] = []
    missing_advisory: collections.Counter[str] = collections.Counter()
    unknown_kinds: collections.Counter[str] = collections.Counter()
    empty_page_mutations: list[dict[str, Any]] = []
    non_monotonic_indices: list[dict[str, Any]] = []
    previous_source_index = -1
    for ordinal, record in enumerate(records):
        kind = str(record.get("transition_kind") or "")
        if kind not in KNOWN_TRANSITION_KINDS:
            unknown_kinds[kind or "<empty>"] += 1
        missing = [field for field in CORE_RECORD_FIELDS if missing_core_field(record, field)]
        if missing and len(missing_required) < sample_limit:
            missing_required.append({"ordinal": ordinal, "missing_fields": missing, "transition_kind": kind})
        for field in ADVISORY_RECORD_FIELDS:
            if missing_core_field(record, field):
                missing_advisory[field] += 1
        pages = record_pages(record)
        if kind and kind != "increment_hit_count" and not pages:
            empty_page_mutations.append(
                {"ordinal": ordinal, "transition_kind": kind, "source_event_index": record.get("source_event_index")}
            )
        source_index = optional_int(record.get("source_event_index"), -1)
        if source_index >= 0 and previous_source_index > source_index:
            non_monotonic_indices.append(
                {
                    "ordinal": ordinal,
                    "previous_source_event_index": previous_source_index,
                    "source_event_index": source_index,
                }
            )
        if source_index >= 0:
            previous_source_index = max(previous_source_index, source_index)
    return {
        "schema_name": predicted.get("schema"),
        "record_count": len(records),
        "unknown_transition_kinds": dict(sorted(unknown_kinds.items())),
        "missing_required_field_count": sum(len(item["missing_fields"]) for item in missing_required),
        "missing_required_fields": missing_required,
        "missing_advisory_field_counts": dict(sorted(missing_advisory.items())),
        "transition_id_present": all("transition_id" in record for record in records),
        "transition_id_note": "predicted_target_cache_state_trace.v1 may omit C++ transition_id; replay uses record ordinal when absent.",
        "empty_page_mutation_count": len(empty_page_mutations),
        "empty_page_mutations": empty_page_mutations[:sample_limit],
        "non_monotonic_source_event_index_count": len(non_monotonic_indices),
        "non_monotonic_source_event_indices": non_monotonic_indices[:sample_limit],
    }


def build_provenance_check(records: list[dict[str, Any]], *, sample_limit: int) -> dict[str, Any]:
    """检查 transition 是否携带基本来源字段。"""

    without_source = []
    without_policy_hint = []
    by_source_event = collections.Counter()
    for ordinal, record in enumerate(records):
        source_index = record.get("source_event_index")
        source_name = str(record.get("source_event_name") or "")
        if source_index is None or not source_name:
            without_source.append(
                {
                    "ordinal": ordinal,
                    "transition_kind": record.get("transition_kind"),
                    "source_event_index": source_index,
                }
            )
        by_source_event[source_name] += 1
        if not str(record.get("decision_reason") or ""):
            without_policy_hint.append({"ordinal": ordinal, "transition_kind": record.get("transition_kind")})
    return {
        "transitions_without_source_fact_count": len(without_source),
        "transitions_without_source_fact": without_source[:sample_limit],
        "transitions_without_policy_decision_hint_count": len(without_policy_hint),
        "transitions_without_policy_decision_hint": without_policy_hint[:sample_limit],
        "transition_count_by_source_event": dict(sorted(by_source_event.items())),
    }


def build_lifecycle_check(records: list[dict[str, Any]], *, sample_limit: int) -> dict[str, Any]:
    """检查粗粒度 lifecycle 是否闭合。"""

    prefetch_by_request: dict[str, collections.Counter[str]] = collections.defaultdict(collections.Counter)
    writeback_pending: collections.Counter[str] = collections.Counter()
    storage_pending: collections.Counter[str] = collections.Counter()
    loadback_pending: collections.Counter[str] = collections.Counter()
    write_through_pending: collections.Counter[str] = collections.Counter()
    for record in records:
        kind = str(record.get("transition_kind") or "")
        request_key = transition_request_key(record)
        if kind.startswith("prefetch_") or kind == "apply_prefetch_host_visibility":
            prefetch_by_request[request_key][kind] += 1
        write_through_key = str(record.get("cache_scope") or "")
        if kind == "enqueue_writeback":
            writeback_pending[request_key] += 1
        elif kind in {"complete_writeback", "cancel_writeback"} and writeback_pending[request_key] > 0:
            writeback_pending[request_key] -= 1
        if kind == "enqueue_storage_backup":
            storage_pending[request_key] += 1
        elif kind == "complete_storage_backup" and storage_pending[request_key] > 0:
            storage_pending[request_key] -= 1
        if kind == "enqueue_loadback":
            loadback_pending[request_key] += 1
        elif kind == "complete_loadback" and loadback_pending[request_key] > 0:
            loadback_pending[request_key] -= 1
        if kind == "enqueue_write_through_backup":
            # write-through backup ref 是 scope 级 pending 队列；C++ 会在后续事实或
            # target finalize 边界 drain。completion 的 request_id 可能来自触发 drain
            # 的新 fact，不能按 request 级闭合。
            write_through_pending[write_through_key] += 1
        elif kind == "complete_write_through_backup" and write_through_pending[write_through_key] > 0:
            write_through_pending[write_through_key] -= 1

    unclosed: list[dict[str, Any]] = []
    for request_key, counts in sorted(prefetch_by_request.items()):
        planned = counts.get("prefetch_planned", 0)
        terminal = (
            counts.get("prefetch_ready", 0)
            + counts.get("prefetch_revoked", 0)
            + counts.get("prefetch_suppressed", 0)
            + counts.get("prefetch_terminated", 0)
            + counts.get("prefetch_timeout_incomplete", 0)
        )
        if terminal < planned:
            unclosed.append(
                {
                    "lifecycle": "prefetch",
                    "request_key": request_key,
                    "planned": planned,
                    "terminal": terminal,
                    "counts": dict(counts),
                }
            )
    for lifecycle, pending in (
        ("writeback", writeback_pending),
        ("storage_backup", storage_pending),
        ("loadback", loadback_pending),
        ("write_through_backup", write_through_pending),
    ):
        for request_key, count in sorted(pending.items()):
            if count > 0:
                unclosed.append({"lifecycle": lifecycle, "request_key": request_key, "pending_count": count})
    return {
        "unclosed_required_lifecycle_count": len(unclosed),
        "unclosed_required_lifecycle": unclosed[:sample_limit],
        "advisory_open_lifecycle_count": 0,
        "advisory_open_lifecycle": [],
        "prefetch_request_count": len(prefetch_by_request),
    }


def transition_request_key(record: dict[str, Any]) -> str:
    """生成 run-local request / operation 归因 key。"""

    return ":".join(
        [
            str(record.get("cache_scope") or ""),
            str(record.get("request_id") or ""),
            str(record.get("operation_id") or ""),
        ]
    )
