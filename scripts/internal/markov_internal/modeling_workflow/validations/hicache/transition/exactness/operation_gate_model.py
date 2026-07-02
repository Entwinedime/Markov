"""模型侧 transition operation gate 构造。"""

from __future__ import annotations

from typing import Any

from ...oracle.snapshot.state import normalize_hicache_page_key
from ..replay.record_schema import record_pages
from .taxonomy_aggregation import append_unique
from .taxonomy_constants import PHYSICAL_CANDIDATE_OPERATION_KINDS, STATE_ONLY_OPERATION_KINDS
from .taxonomy_evidence import list_dicts


def canonical_request_key_from_row(row: dict[str, Any]) -> str:
    """生成保守的 run-local canonical request key。"""

    request_id = str(row.get("request_id") or "")
    cache_scope = str(row.get("cache_scope") or "")
    operation_id = str(row.get("operation_id") or "")
    if request_id:
        return f"{cache_scope}:{request_id}"
    if operation_id:
        return f"{cache_scope}:operation:{operation_id}"
    return cache_scope


def build_model_operation_gates(
    records: list[dict[str, Any]],
    hicache_summary: dict[str, Any],
    prediction_entry: dict[str, Any],
    *,
    page_key_mode: str,
    sample_limit: int,
) -> list[dict[str, Any]]:
    """从模型 transition trace 聚合 patch gate 诊断行。"""

    del sample_limit
    operation_provenance = collect_operation_provenance(hicache_summary)
    grouped: dict[str, dict[str, Any]] = {}
    for ordinal, record in enumerate(records):
        transition_kind = str(record.get("transition_kind") or "")
        operation_kind = operation_gate_kind_from_transition(transition_kind)
        classification = operation_gate_classification(operation_kind)
        pages = sorted({normalize_hicache_page_key(page, page_key_mode) for page in record_pages(record)})
        operation_id = str(record.get("operation_id") or "")
        grouping_key = operation_gate_grouping_key(record, operation_kind, ordinal)
        item = grouped.setdefault(
            grouping_key,
            {
                "gate_id": f"diagnostic:model:{len(grouped) + 1}",
                "operation_kind": operation_kind,
                "gate_maturity": "diagnostic",
                "patch_allowed": False,
                "operation_class": operation_gate_class(operation_kind),
                "cache_scope": record.get("cache_scope") or "",
                "request_key": canonical_request_key_from_row(record),
                "operation_id": operation_id,
                "pages": [],
                "page_count": 0,
                "transition_family": prediction_entry.get("family"),
                "classification": classification,
                "patch_risk": prediction_entry.get("patch_risk"),
                "grouping_confidence": "strong" if operation_id else "weak",
                "provenance": {
                    "transition_ids": [],
                    "transition_ordinals": [],
                    "transition_kinds": [],
                    "policy_decision_epochs": [],
                    "async_lifecycle_epochs": [],
                    "capacity_victim_epochs": [],
                    "ref_mutation_epochs": [],
                    "observed_event_ids": [],
                },
            },
        )
        item["pages"] = sorted(set(item["pages"]) | set(pages))
        item["page_count"] = len(item["pages"])
        transition_id = record.get("transition_id") or ordinal
        item["provenance"]["transition_ids"].append(str(transition_id))
        item["provenance"]["transition_ordinals"].append(ordinal)
        append_unique(item["provenance"]["transition_kinds"], transition_kind)
        if operation_id:
            merge_operation_provenance(item["provenance"], operation_provenance.get(operation_id, {}))
    return sorted(grouped.values(), key=lambda row: str(row.get("gate_id") or ""))


def collect_operation_provenance(hicache_summary: dict[str, Any]) -> dict[str, dict[str, list[Any]]]:
    """按 operation_id 汇总 C++ 账本 provenance。"""

    provenance: dict[str, dict[str, list[Any]]] = {}
    ledger_specs = (
        ("policy_decision_trace", "policy_decision_epochs", "decision_epoch"),
        ("async_lifecycle_trace", "async_lifecycle_epochs", "transition_epoch"),
        ("capacity_victim_choices", "capacity_victim_epochs", "selection_epoch"),
        ("ref_mutation_trace", "ref_mutation_epochs", "mutation_epoch"),
    )
    for ledger_name, output_key, epoch_key in ledger_specs:
        for row in list_dicts(hicache_summary.get(ledger_name, [])):
            operation_id = str(row.get("operation_id") or "")
            if not operation_id:
                continue
            item = provenance.setdefault(operation_id, {})
            append_unique(item.setdefault(output_key, []), row.get(epoch_key))
    return provenance


def merge_operation_provenance(target: dict[str, list[Any]], source: dict[str, list[Any]]) -> None:
    """把 operation-level provenance 合并到 gate provenance。"""

    for key, values in source.items():
        if not isinstance(values, list):
            continue
        out = target.setdefault(key, [])
        for value in values:
            append_unique(out, value)


def operation_gate_grouping_key(record: dict[str, Any], operation_kind: str, ordinal: int) -> str:
    """生成 operation gate grouping key。"""

    operation_id = str(record.get("operation_id") or "")
    cache_scope = str(record.get("cache_scope") or "")
    if operation_id:
        return f"{cache_scope}:op:{operation_id}:{operation_kind}"
    request_key = canonical_request_key_from_row(record)
    source_event_index = str(record.get("source_event_index") or "")
    event_name = str(record.get("source_event_name") or record.get("event_base_name") or "")
    if request_key or source_event_index or event_name:
        return f"{cache_scope}:weak:{request_key}:{source_event_index}:{event_name}:{operation_kind}"
    return f"{cache_scope}:weak:ordinal:{ordinal}:{operation_kind}"


def operation_gate_kind_from_transition(transition_kind: str) -> str:
    """把模型 transition kind 映射到 patch gate operation taxonomy。"""

    if transition_kind in {"mark_dirty", "clear_dirty"}:
        return "dirty_marker"
    if transition_kind in {"mark_evicted", "clear_evicted"}:
        return "evicted_marker"
    if transition_kind in {"mark_backuped", "clear_backuped"}:
        return "backuped_marker"
    if transition_kind in {"cache_extend_acquire_request_ref", "release_request_ref"}:
        return "ref_protection"
    if transition_kind == "increment_hit_count":
        return "hit_count_update"
    if transition_kind in {"add_l1_residency", "restore_l1_residency"}:
        return "request_insert"
    if transition_kind in {"promote_visible_prefix_to_l1", "enqueue_loadback", "complete_loadback"}:
        return "device_loadback"
    if transition_kind == "evict_l1_node":
        return "device_eviction"
    if transition_kind == "evict_host_node":
        return "host_cleanup"
    if transition_kind in {"enqueue_write_through_backup", "complete_write_through_backup", "commit_host_backup"}:
        return "host_backup"
    if transition_kind in {"enqueue_storage_backup", "commit_host_storage_backup", "complete_storage_backup"}:
        return "storage_backup"
    if transition_kind in {"enqueue_writeback", "complete_writeback", "cancel_writeback"}:
        return "write_back_flush"
    if transition_kind == "prefetch_planned":
        return "prefetch_plan"
    if transition_kind == "prefetch_ready":
        return "prefetch_read"
    if transition_kind == "apply_prefetch_host_visibility":
        return "prefetch_apply"
    if transition_kind == "prefetch_terminated":
        return "prefetch_control"
    if transition_kind in {"prefetch_revoked", "prefetch_suppressed", "prefetch_timeout_incomplete"}:
        return "prefetch_revoke"
    return "unresolved"


def operation_gate_class(operation_kind: str) -> str:
    """返回 operation gate 的粗粒度 class。"""

    if operation_kind in {"write_back_flush", "storage_backup", "prefetch_read"}:
        return "physical_io"
    if operation_kind in {"host_backup", "device_loadback"}:
        return "physical_memory"
    if operation_kind in {"host_cleanup", "device_eviction", "prefetch_apply", "request_insert"}:
        return "metadata_control"
    if operation_kind in STATE_ONLY_OPERATION_KINDS:
        return "state_only"
    return "unknown"


def operation_gate_classification(operation_kind: str) -> str:
    """返回 patch gate coverage 使用的 classification。"""

    if operation_kind in STATE_ONLY_OPERATION_KINDS:
        return "state_marker_only"
    if operation_kind in PHYSICAL_CANDIDATE_OPERATION_KINDS:
        return "physical_candidate"
    if operation_kind == "request_insert":
        return "metadata_candidate"
    return "unresolved"
