"""transition family 分类使用的证据摘要辅助工具。"""

from __future__ import annotations

import collections
from pathlib import Path
from typing import Any

from markov_internal.common.io import load_json
from ...oracle.snapshot.state import normalize_hicache_page_key
from .taxonomy_constants import TRANSITION_PAGE_FIELDS


def load_hicache_summary(path: Path) -> dict[str, Any]:
    """读取 model_summary 中的 HiCache summary。"""

    if not path.is_file():
        return {}
    payload = load_json(path)
    modules = payload.get("modules") if isinstance(payload, dict) else None
    if not isinstance(modules, list):
        return {}
    for module in modules:
        if isinstance(module, dict) and isinstance(module.get("hicache"), dict):
            return module["hicache"]
    return {}


def summarize_hicache_evidence(
    hicache_summary: dict[str, Any],
    sample_pages: list[str],
    *,
    page_key_mode: str,
    sample_limit: int,
) -> dict[str, Any]:
    """从 C++ HiCache summary 抽取 family 诊断所需证据摘要。"""

    ledgers = {
        "transition_trace": hicache_summary.get("transition_trace", []),
        "policy_decision_trace": hicache_summary.get("policy_decision_trace", []),
        "async_lifecycle_trace": hicache_summary.get("async_lifecycle_trace", []),
        "capacity_mutation_trace": hicache_summary.get("capacity_mutation_trace", []),
        "capacity_victim_choices": hicache_summary.get("capacity_victim_choices", []),
        "ref_mutation_trace": hicache_summary.get("ref_mutation_trace", []),
        "control_boundary_trace": hicache_summary.get("control_boundary_trace", []),
        "radix_split_trace": hicache_summary.get("radix_split_trace", []),
    }
    transition_rows = list_dicts(ledgers["transition_trace"])
    policy_rows = list_dicts(ledgers["policy_decision_trace"])
    async_rows = list_dicts(ledgers["async_lifecycle_trace"])
    victim_rows = list_dicts(ledgers["capacity_victim_choices"])
    ref_rows = list_dicts(ledgers["ref_mutation_trace"])
    sample_page_set = {normalize_hicache_page_key(page, page_key_mode) for page in sample_pages}
    return {
        "target_config": hicache_summary.get("target_config", {}),
        "resolved_policy": hicache_summary.get("resolved_policy", {}),
        "state_transition_count": hicache_summary.get("state_transition_count", len(transition_rows)),
        "transition_count_by_kind": count_by_field(transition_rows, "kind"),
        "policy_decision_count": hicache_summary.get("policy_decision_count", len(policy_rows)),
        "policy_area_counts": count_by_field(policy_rows, "policy_area"),
        "policy_decision_counts": count_by_field(policy_rows, "decision"),
        "async_lifecycle_transition_count": hicache_summary.get("async_lifecycle_transition_count", len(async_rows)),
        "async_kind_counts": count_by_field(async_rows, "kind"),
        "async_state_counts": count_by_field(async_rows, "to_state"),
        "capacity_victim_choice_count": hicache_summary.get("capacity_victim_choice_count", len(victim_rows)),
        "capacity_victim_by_tier": count_by_field(victim_rows, "tier"),
        "capacity_victim_by_reason": count_by_field(victim_rows, "reason"),
        "ref_mutation_count": hicache_summary.get("ref_mutation_count", len(ref_rows)),
        "ref_owner_kind_counts": count_by_field(ref_rows, "owner_kind"),
        "control_boundary_count": hicache_summary.get(
            "control_boundary_count", list_len(ledgers["control_boundary_trace"])
        ),
        "radix_split_count": hicache_summary.get("radix_split_count", list_len(ledgers["radix_split_trace"])),
        "warnings": hicache_summary.get("warnings", []),
        "sample_policy_decisions": sample_rows_by_pages(
            policy_rows, sample_page_set, page_key_mode=page_key_mode, sample_limit=sample_limit
        ),
        "sample_async_lifecycle": sample_rows_by_pages(
            async_rows, sample_page_set, page_key_mode=page_key_mode, sample_limit=sample_limit
        ),
        "sample_capacity_victims": sample_rows_by_pages(
            victim_rows, sample_page_set, page_key_mode=page_key_mode, sample_limit=sample_limit
        ),
        "sample_ref_mutations": sample_rows_by_pages(
            ref_rows, sample_page_set, page_key_mode=page_key_mode, sample_limit=sample_limit
        ),
    }


def summarize_observed_target_evidence(
    observed_path: Path,
    sample_pages: list[str],
    *,
    page_key_mode: str,
    sample_limit: int,
) -> dict[str, Any]:
    """从 observed target oracle 抽取 validation-only evidence 摘要。"""

    if not observed_path.is_file():
        return {"oracle_ready": False, "reason": "missing observed target trace"}
    observed = load_json(observed_path)
    operations = list_dicts(observed.get("observed_operations", []))
    deltas = list_dicts(observed.get("snapshot_delta_rows", []))
    sample_page_set = {normalize_hicache_page_key(page, page_key_mode) for page in sample_pages}
    return {
        "oracle_ready": bool(observed.get("oracle_ready")),
        "observed_operation_count": len(operations),
        "snapshot_delta_count": len(deltas),
        "operation_kind_counts": count_by_field(operations, "operation_kind"),
        "fact_role_counts": count_by_field(operations, "fact_role"),
        "snapshot_delta_kind_counts": count_by_field(deltas, "transition_kind"),
        "sample_observed_operations": sample_rows_by_pages(
            operations, sample_page_set, page_key_mode=page_key_mode, sample_limit=sample_limit
        ),
        "sample_snapshot_deltas": sample_rows_by_pages(
            deltas, sample_page_set, page_key_mode=page_key_mode, sample_limit=sample_limit
        ),
        "unsupported_or_unobservable_state_keys": observed.get("unsupported_or_unobservable_state_keys", []),
    }


def list_dicts(value: Any) -> list[dict[str, Any]]:
    """过滤出 dict 列表。"""

    if not isinstance(value, list):
        return []
    return [item for item in value if isinstance(item, dict)]


def list_len(value: Any) -> int:
    """安全读取列表长度。"""

    return len(value) if isinstance(value, list) else 0


def count_by_field(rows: list[dict[str, Any]], field: str) -> dict[str, int]:
    """按字段计数。"""

    return dict(sorted(collections.Counter(str(row.get(field) or "") for row in rows).items()))


def sample_rows_by_pages(
    rows: list[dict[str, Any]],
    sample_pages: set[str],
    *,
    page_key_mode: str,
    sample_limit: int,
) -> list[dict[str, Any]]:
    """优先抽取和 sample pages 相交的 ledger rows。"""

    if not rows:
        return []
    matched = [compact_evidence_row(row) for row in rows if row_intersects_pages(row, sample_pages, page_key_mode)]
    if matched:
        return matched[:sample_limit]
    return [compact_evidence_row(row) for row in rows[:sample_limit]]


def row_intersects_pages(row: dict[str, Any], sample_pages: set[str], page_key_mode: str) -> bool:
    """判断 row 是否和 sample pages 相交。"""

    if not sample_pages:
        return False
    for field in TRANSITION_PAGE_FIELDS:
        pages = row.get(field)
        if not isinstance(pages, list):
            continue
        normalized = {normalize_hicache_page_key(page, page_key_mode) for page in pages if page is not None}
        if normalized & sample_pages:
            return True
    return False


def compact_evidence_row(row: dict[str, Any]) -> dict[str, Any]:
    """压缩 evidence row，避免 catalog 过大。"""

    keep_fields = (
        "transition_id",
        "kind",
        "transition_kind",
        "role",
        "event_name",
        "fact_role",
        "event_kind",
        "source_event_index",
        "operation_id",
        "request_key",
        "request_id",
        "cache_scope",
        "policy_area",
        "policy_name",
        "decision",
        "reason",
        "accepted",
        "requested_pages",
        "allocated_pages",
        "capacity_pages",
        "occupied_pages",
        "tier",
        "selection_epoch",
        "mutation_epoch",
        "transition_epoch",
        "to_state",
        "from_state",
        "owner_kind",
        "action",
        "page_count",
        "pages",
        "host_pages",
        "lock_pages",
    )
    result = {field: row.get(field) for field in keep_fields if field in row}
    for field in ("pages", "host_pages", "lock_pages"):
        if isinstance(result.get(field), list) and len(result[field]) > 12:
            result[field] = result[field][:12] + [f"...({len(result[field])} total)"]
    return result
