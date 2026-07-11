"""Model-side transition operation-gate construction."""

from __future__ import annotations

from typing import Any

from ...oracle.snapshot.state import normalize_hicache_page_key
from ..replay.record_schema import record_pages
from .request_identity import canonical_request_key
from .taxonomy_aggregation import append_unique
from .taxonomy_constants import PHYSICAL_CANDIDATE_OPERATION_KINDS, STATE_ONLY_OPERATION_KINDS
from .taxonomy_evidence import list_dicts


OPERATION_KIND_BY_TRANSITION = {
    "add_l1_residency": "request_insert",
    "apply_prefetch_host_visibility": "prefetch_apply",
    "cache_extend_acquire_request_ref": "ref_protection",
    "cancel_writeback": "write_back_flush",
    "clear_backuped": "backuped_marker",
    "clear_dirty": "dirty_marker",
    "clear_evicted": "evicted_marker",
    "commit_host_backup": "host_backup",
    "commit_host_storage_backup": "storage_backup",
    "complete_loadback": "device_loadback",
    "complete_storage_backup": "storage_backup",
    "complete_write_through_backup": "host_backup",
    "complete_writeback": "write_back_flush",
    "enqueue_loadback": "device_loadback",
    "enqueue_storage_backup": "storage_backup",
    "enqueue_write_through_backup": "host_backup",
    "enqueue_writeback": "write_back_flush",
    "evict_host_node": "host_cleanup",
    "evict_l1_node": "device_eviction",
    "increment_hit_count": "hit_count_update",
    "mark_backuped": "backuped_marker",
    "mark_dirty": "dirty_marker",
    "mark_evicted": "evicted_marker",
    "prefetch_planned": "prefetch_plan",
    "prefetch_ready": "prefetch_read",
    "prefetch_revoked": "prefetch_revoke",
    "prefetch_suppressed": "prefetch_revoke",
    "prefetch_terminated": "prefetch_control",
    "prefetch_timeout_incomplete": "prefetch_revoke",
    "promote_visible_prefix_to_l1": "device_loadback",
    "release_request_ref": "ref_protection",
    "restore_l1_residency": "request_insert",
}


def build_model_operation_gates(
    records: list[dict[str, Any]],
    hicache_summary: dict[str, Any],
    prediction_entry: dict[str, Any],
    *,
    page_key_mode: str,
) -> list[dict[str, Any]]:
    """Aggregate model transitions into diagnostic patch-gate operations."""

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
                "request_key": canonical_request_key(record),
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
    """Index C++ ledger provenance by operation identifier."""

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
    """Merge operation-level ledger provenance into one gate."""

    for key, values in source.items():
        if not isinstance(values, list):
            continue
        out = target.setdefault(key, [])
        for value in values:
            append_unique(out, value)


def operation_gate_grouping_key(record: dict[str, Any], operation_kind: str, ordinal: int) -> str:
    """Build the strongest available operation-gate grouping key."""

    operation_id = str(record.get("operation_id") or "")
    cache_scope = str(record.get("cache_scope") or "")
    if operation_id:
        return f"{cache_scope}:op:{operation_id}:{operation_kind}"
    request_key = canonical_request_key(record)
    source_event_index_value = record.get("source_event_index")
    source_event_index = "" if source_event_index_value is None else str(source_event_index_value)
    event_name = str(record.get("source_event_name") or record.get("event_base_name") or "")
    if request_key or source_event_index or event_name:
        return f"{cache_scope}:weak:{request_key}:{source_event_index}:{event_name}:{operation_kind}"
    return f"{cache_scope}:weak:ordinal:{ordinal}:{operation_kind}"


def operation_gate_kind_from_transition(transition_kind: str) -> str:
    """Map a model transition to the patch-gate operation taxonomy."""

    return OPERATION_KIND_BY_TRANSITION.get(transition_kind, "unresolved")


def operation_gate_class(operation_kind: str) -> str:
    """Return the coarse physical/control class of an operation gate."""

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
    """Return the classification consumed by gate-coverage checks."""

    if operation_kind in STATE_ONLY_OPERATION_KINDS:
        return "state_marker_only"
    if operation_kind in PHYSICAL_CANDIDATE_OPERATION_KINDS:
        return "physical_candidate"
    if operation_kind == "request_insert":
        return "metadata_candidate"
    return "unresolved"
