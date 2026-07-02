"""target-side observed operation gate 构造。"""

from __future__ import annotations

from typing import Any

from ...oracle.snapshot.state import normalize_hicache_page_key
from .operation_gate_model import operation_gate_class, operation_gate_classification
from .taxonomy_aggregation import append_unique
from .taxonomy_constants import OBSERVED_ROLE_TO_OPERATION_KIND
from .taxonomy_evidence import list_dicts


def build_observed_operation_gates(
    observed: dict[str, Any],
    prediction_entry: dict[str, Any],
    *,
    page_key_mode: str,
    sample_limit: int,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """把 target-side observed operations 聚合成 validation-only operation gate。"""

    operations = list_dicts(observed.get("observed_operations", []))
    grouped: dict[str, dict[str, Any]] = {}
    for ordinal, row in enumerate(operations):
        operation_kind = operation_gate_kind_from_observed(row)
        pages = row.get("pages") if isinstance(row.get("pages"), list) else []
        normalized_pages = sorted(
            {normalize_hicache_page_key(page, page_key_mode) for page in pages if page is not None}
        )
        operation_id = str(row.get("operation_id") or "")
        request_key = str(row.get("canonical_request_key") or "")
        cache_scope = str(row.get("cache_scope") or "")
        grouping_key = f"{cache_scope}:{request_key}:{operation_id}:{operation_kind}:{row.get('fact_role') or ''}"
        item = grouped.setdefault(
            grouping_key,
            {
                "gate_id": f"diagnostic:observed:{len(grouped) + 1}",
                "operation_kind": operation_kind,
                "gate_maturity": "diagnostic",
                "patch_allowed": False,
                "operation_class": operation_gate_class(operation_kind),
                "cache_scope": cache_scope,
                "request_key": request_key,
                "operation_id": operation_id,
                "pages": [],
                "page_count": 0,
                "transition_family": prediction_entry.get("family"),
                "classification": operation_gate_classification(operation_kind),
                "patch_risk": prediction_entry.get("patch_risk"),
                "evidence_class": observed_evidence_class(row),
                "provenance": {
                    "observed_event_ids": [],
                    "fact_roles": [],
                    "event_kinds": [],
                    "transition_ids": [],
                    "policy_decision_epochs": [],
                    "async_lifecycle_epochs": [],
                    "capacity_victim_epochs": [],
                    "ref_mutation_epochs": [],
                },
            },
        )
        item["pages"] = sorted(set(item["pages"]) | set(normalized_pages))
        item["page_count"] = len(item["pages"])
        append_unique(
            item["provenance"]["observed_event_ids"], row.get("observed_operation_id") or ordinal, limit=sample_limit
        )
        append_unique(item["provenance"]["fact_roles"], row.get("fact_role"))
        append_unique(item["provenance"]["event_kinds"], row.get("event_kind"))
    return sorted(grouped.values(), key=lambda item: str(item.get("gate_id") or "")), {
        "input_operation_count": len(operations),
        "emitted_group_count": len(grouped),
    }


def operation_gate_kind_from_observed(row: dict[str, Any]) -> str:
    """把 observed operation 规整为 patch gate operation kind。"""

    operation_kind = str(row.get("operation_kind") or "")
    fact_role = str(row.get("fact_role") or "")
    mapped = OBSERVED_ROLE_TO_OPERATION_KIND.get(fact_role) or operation_kind or "unknown"
    if mapped in {"capacity_request", "capacity_result"}:
        return "allocator_pressure"
    if mapped in {"lock_ref"}:
        return "ref_protection"
    if mapped in {"write_through_backup"}:
        return "host_backup"
    if mapped == "prefetch_ready":
        return "prefetch_read"
    return mapped


def observed_evidence_class(row: dict[str, Any]) -> str:
    """标记 observed operation evidence 的证据等级。"""

    fact_class = str(row.get("fact_class") or "")
    fact_role = str(row.get("fact_role") or "")
    if fact_class == "source_actual" and fact_role.endswith("_observed"):
        return "exact_physical"
    if fact_role in {"capacity_request", "capacity_result_observed"}:
        return "diagnostic_anchor"
    if "snapshot" in fact_role:
        return "snapshot_delta"
    return "physical_boundary"
