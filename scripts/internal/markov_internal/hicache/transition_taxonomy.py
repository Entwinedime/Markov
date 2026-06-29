#!/usr/bin/env python3
"""HiCache transition family 分类与诊断证据辅助函数。

本模块只负责 transition compare 结果的 family 分类、DAG patch gate 字段和
catalog 需要的证据摘要。它不读取 profile manifest，也不执行 matrix compare。
"""

from __future__ import annotations

import collections
from pathlib import Path
from typing import Any

from ..common.io import load_json
from .oracle_state import normalize_hicache_page_key


def build_transition_classification_entry(
    context: dict[str, Any],
    comparison: dict[str, Any],
    hicache_summary: dict[str, Any],
    observed_path: Path,
    *,
    page_key_mode: str,
    sample_limit: int,
    include_evidence: bool,
) -> dict[str, Any]:
    """把单次 compare 结果规整成 transition family 和 patch gate。"""

    mismatch_kinds = transition_mismatch_kinds(comparison)
    family, reason = classify_transition_family(context, comparison, hicache_summary, mismatch_kinds)
    review = review_for_transition_family(family)
    gate = dag_patch_gate_fields(str(review.get("classification") or ""), family)
    sample_mismatches = comparison.get("page_lifecycle_multiset_comparison", {}).get("top_mismatches", [])
    sample_pages = sorted(
        {
            str(item.get("page"))
            for item in sample_mismatches
            if isinstance(item, dict) and item.get("page") is not None
        }
    )[:sample_limit]
    count_comparison = comparison.get("transition_count_comparison", {}).get("by_kind", {})
    lifecycle_comparison = comparison.get("page_lifecycle_multiset_comparison", {})
    entry = {
        "label": context.get("label"),
        "input_id": context.get("input_id"),
        "source_config_id": context.get("source_config_id"),
        "target_config_id": context.get("target_config_id"),
        "source_run_id": context.get("source_run_id"),
        "target_run_id": context.get("target_run_id"),
        "is_self": context.get("is_self"),
        "prediction_dir": context.get("prediction_dir"),
        "transition_exactness_path": context.get("transition_exactness_path"),
        "observed_target_trace_path": str(observed_path),
        "ready": bool(comparison.get("ready")),
        "exact": bool(comparison.get("exact")),
        "final_state_exact": bool(comparison.get("final_state_exact")),
        "transition_count_exact": bool(comparison.get("transition_count_exact")),
        "page_lifecycle_multiset_exact": bool(comparison.get("page_lifecycle_multiset_exact")),
        "failure_classification": comparison.get("failure_classification"),
        "family": family,
        "classification": review.get("classification"),
        "classification_reason": reason,
        "status": review.get("status"),
        "patch_risk": review.get("patch_risk"),
        "patch_allowed": False,
        "patch_filter_action": gate["patch_filter_action"],
        "source_attribution_required": gate["source_attribution_required"],
        "duration_required": gate["duration_required"],
        "evidence_required": gate["evidence_required"],
        "mismatch_kinds": mismatch_kinds,
        "transition_count_diffs": {
            kind: value
            for kind, value in count_comparison.items()
            if isinstance(value, dict) and not value.get("match")
        },
        "mismatch_totals_by_kind": lifecycle_comparison.get("mismatch_totals_by_kind", {}),
        "sample_pages": sample_pages,
        "sample_mismatches": sample_mismatches[:sample_limit] if isinstance(sample_mismatches, list) else [],
        "model_delta_count_by_kind": comparison.get("model_delta_count_by_kind", {}),
        "observed_delta_count_by_kind": comparison.get("observed_delta_count_by_kind", {}),
    }
    if include_evidence:
        entry["mechanism_review"] = review
        entry["hicache_evidence"] = summarize_hicache_evidence(
            hicache_summary,
            sample_pages,
            page_key_mode=page_key_mode,
            sample_limit=sample_limit,
        )
        entry["observed_evidence"] = summarize_observed_target_evidence(
            observed_path,
            sample_pages,
            page_key_mode=page_key_mode,
            sample_limit=sample_limit,
        )
    return entry


def compare_result_classification_fields(classification_entry: dict[str, Any]) -> dict[str, Any]:
    """抽取 compare 主结果中的分类与 gate 字段。"""

    return {
        "transition_family": classification_entry.get("family"),
        "classification": classification_entry.get("classification"),
        "classification_reason": classification_entry.get("classification_reason"),
        "mismatch_kinds": classification_entry.get("mismatch_kinds", []),
        "patch_gate": {
            "patch_allowed": False,
            "patch_filter_action": classification_entry.get("patch_filter_action"),
            "patch_risk": classification_entry.get("patch_risk"),
            "status": classification_entry.get("status"),
            "source_attribution_required": classification_entry.get("source_attribution_required"),
            "duration_required": classification_entry.get("duration_required"),
            "evidence_required": classification_entry.get("evidence_required", []),
        },
    }


MARKER_DELTA_KINDS = {
    "mark_dirty",
    "clear_dirty",
    "mark_backuped",
    "clear_backuped",
    "mark_evicted",
    "clear_evicted",
    "mark_locked",
    "clear_locked",
    "mark_pending_writeback",
    "clear_pending_writeback",
}

PREFETCH_DELTA_KINDS = {
    "prefetch_planned",
    "clear_prefetch_planned",
    "prefetch_ready",
    "clear_prefetch_ready",
    "prefetch_late",
    "clear_prefetch_late",
    "prefetch_suppressed",
    "clear_prefetch_suppressed",
}

HOST_VISIBLE_DELTA_KINDS = {
    "add_l2_resident",
    "remove_l2_resident",
    "add_l3_resident",
    "remove_l3_resident",
    "mark_backuped",
    "clear_backuped",
}

DEVICE_VISIBLE_DELTA_KINDS = {
    "add_l1_resident",
    "remove_l1_resident",
}

TRANSITION_PAGE_FIELDS = (
    "pages",
    "target_page_set",
    "host_pages",
    "lock_pages",
    "prefix_pages",
    "suffix_pages",
    "hit_pages",
)

NOISE_OBSERVED_OPERATION_KINDS = {
    "maintenance_checkpoint",
    "request_lookup",
    "request_lifecycle",
}

STATE_ONLY_OPERATION_KINDS = {
    "dirty_marker",
    "evicted_marker",
    "backuped_marker",
    "ref_protection",
    "hit_count_update",
    "allocator_pressure",
    "prefetch_control",
    "prefetch_plan",
    "prefetch_revoke",
    "snapshot_delta_marker",
}

PHYSICAL_CANDIDATE_OPERATION_KINDS = {
    "host_backup",
    "storage_backup",
    "write_back_flush",
    "device_loadback",
    "prefetch_read",
    "prefetch_apply",
    "host_cleanup",
    "device_eviction",
}

def family_review(
    status: str,
    patch_risk: str,
    classification: str,
    observed_evidence: str,
    recommended_fix: str,
) -> dict[str, Any]:
    """生成 catalog 使用的精简 family review。"""

    return {
        "status": status,
        "patch_risk": patch_risk,
        "classification": classification,
        "sglang_source_anchor": [],
        "model_call_site": [],
        "observed_evidence": observed_evidence,
        "recommended_fix": recommended_fix,
    }


FAMILY_REVIEWS = {
    "transition_exact": family_review(
        "state_only",
        "none",
        "matched",
        "Final state and transition deltas match.",
        "No model fix or DAG patch action is required.",
    ),
    "evicted_marker_oscillation": family_review(
        "state_only",
        "low",
        "state_marker_only",
        "Only evicted marker deltas differ.",
        "Keep this out of DAG patching unless a physical eviction gate also diverges.",
    ),
    "dirty_oscillation": family_review(
        "state_only",
        "low",
        "state_marker_only",
        "Only dirty marker deltas differ.",
        "Check backup/writeback gates before treating it as a physical mismatch.",
    ),
    "dirty_evicted_marker_oscillation": family_review(
        "transition_grouping",
        "medium",
        "transition_grouping",
        "Dirty and evicted marker boundaries shift together.",
        "Use physical gate evidence to decide whether C++ lifecycle grouping needs repair.",
    ),
    "writeback_eviction_interleaving": family_review(
        "physical_candidate",
        "high",
        "physical_candidate",
        "Write-back and device eviction deltas may be interleaved differently.",
        "Compare writeback and device-eviction gate evidence before DAG patching.",
    ),
    "host_cleanup_evicted_marker_boundary": family_review(
        "transition_grouping",
        "medium",
        "transition_grouping",
        "Low-host targets shift evicted marker boundaries around cleanup.",
        "Promote only if host-cleanup gate evidence diverges.",
    ),
    "low_host_cleanup_loadback_transient": family_review(
        "physical_candidate",
        "high",
        "physical_candidate",
        "Host cleanup, loadback, and host visibility deltas diverge together.",
        "Split host cleanup, loadback, and prefetch-apply gates before patching.",
    ),
    "prefetch_visibility": family_review(
        "async_visibility_issue",
        "medium",
        "async_visibility",
        "Prefetch visible-state deltas differ.",
        "Keep this as async visibility until storage progress evidence is available.",
    ),
    "model_or_oracle_not_ready": family_review(
        "observed_gap",
        "blocked",
        "observed_unobservable",
        "Model self-check, oracle, or final-state hard gate is not ready.",
        "Fix readiness before transition classification.",
    ),
    "unresolved_transition_mismatch": family_review(
        "observed_gap",
        "blocked",
        "observed_unobservable",
        "No current family rule explains the mismatch shape.",
        "Inspect samples, then fix the model or add a source-backed family rule.",
    ),
}


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


def transition_mismatch_kinds(comparison: dict[str, Any]) -> list[str]:
    """提取单格 transition mismatch kind 集合。"""

    kinds = set()
    lifecycle = comparison.get("page_lifecycle_multiset_comparison", {}) if isinstance(comparison, dict) else {}
    if isinstance(lifecycle, dict):
        totals = lifecycle.get("mismatch_totals_by_kind")
        if isinstance(totals, dict):
            kinds.update(str(kind) for kind in totals)
    count = comparison.get("transition_count_comparison", {}) if isinstance(comparison, dict) else {}
    by_kind = count.get("by_kind") if isinstance(count, dict) else {}
    if isinstance(by_kind, dict):
        for kind, value in by_kind.items():
            if isinstance(value, dict) and not value.get("match"):
                kinds.add(str(kind))
    return sorted(kinds)


def classify_transition_family(
    matrix_row: dict[str, Any],
    comparison: dict[str, Any],
    hicache_summary: dict[str, Any],
    mismatch_kinds: list[str],
) -> tuple[str, str]:
    """把 transition mismatch kind 组合规整成阶段一 family。"""

    del hicache_summary
    target_config = str(matrix_row.get("target_config_id") or comparison.get("target_config_id") or "")
    kind_set = set(mismatch_kinds)
    if not comparison or not comparison.get("ready"):
        return "model_or_oracle_not_ready", "transition exactness output is missing or not ready"
    if not comparison.get("final_state_exact"):
        return "model_or_oracle_not_ready", "final-state hard gate is not exact; transition catalog must not continue"
    if comparison.get("exact") and not kind_set:
        return "transition_exact", "final state, transition count and page lifecycle are exact"
    if kind_set & PREFETCH_DELTA_KINDS:
        return "prefetch_visibility", "prefetch visible state delta mismatch"
    if target_config.startswith("c3_"):
        if kind_set & (HOST_VISIBLE_DELTA_KINDS | DEVICE_VISIBLE_DELTA_KINDS):
            return "low_host_cleanup_loadback_transient", "low-host target has host/device visible delta mismatch"
        if kind_set <= {"mark_evicted", "clear_evicted"}:
            return "host_cleanup_evicted_marker_boundary", "low-host target only diverges in evicted marker lifecycle"
    if target_config.startswith("c2_"):
        if kind_set <= {"mark_dirty", "clear_dirty", "mark_evicted", "clear_evicted"}:
            return "writeback_eviction_interleaving", "write-back target dirty/evicted lifecycle mismatch"
    if target_config.startswith("c1_"):
        if kind_set <= {"mark_dirty", "clear_dirty"}:
            return "dirty_oscillation", "write-through-selective target dirty marker lifecycle mismatch"
        if kind_set <= {"mark_dirty", "clear_dirty", "mark_evicted", "clear_evicted"}:
            return "dirty_evicted_marker_oscillation", "write-through-selective dirty marker also shifts evicted marker boundary"
    if kind_set <= {"mark_evicted", "clear_evicted"}:
        return "evicted_marker_oscillation", "only evicted marker lifecycle differs"
    if kind_set <= {"mark_dirty", "clear_dirty"}:
        return "dirty_oscillation", "only dirty marker lifecycle differs"
    if kind_set <= MARKER_DELTA_KINDS:
        return "dirty_evicted_marker_oscillation", "only marker deltas differ, but kind combination is not target-specific"
    return "unresolved_transition_mismatch", "no family rule matched this transition mismatch shape"


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
        "control_checkpoint_trace": hicache_summary.get("control_checkpoint_trace", []),
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
        "control_checkpoint_count": hicache_summary.get("control_checkpoint_count", list_len(ledgers["control_checkpoint_trace"])),
        "radix_split_count": hicache_summary.get("radix_split_count", list_len(ledgers["radix_split_trace"])),
        "warnings": hicache_summary.get("warnings", []),
        "sample_policy_decisions": sample_rows_by_pages(policy_rows, sample_page_set, page_key_mode=page_key_mode, sample_limit=sample_limit),
        "sample_async_lifecycle": sample_rows_by_pages(async_rows, sample_page_set, page_key_mode=page_key_mode, sample_limit=sample_limit),
        "sample_capacity_victims": sample_rows_by_pages(victim_rows, sample_page_set, page_key_mode=page_key_mode, sample_limit=sample_limit),
        "sample_ref_mutations": sample_rows_by_pages(ref_rows, sample_page_set, page_key_mode=page_key_mode, sample_limit=sample_limit),
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
        "sample_observed_operations": sample_rows_by_pages(operations, sample_page_set, page_key_mode=page_key_mode, sample_limit=sample_limit),
        "sample_snapshot_deltas": sample_rows_by_pages(deltas, sample_page_set, page_key_mode=page_key_mode, sample_limit=sample_limit),
        "unsupported_or_unobservable_state_keys": observed.get("unsupported_or_unobservable_state_keys", []),
    }


def aggregate_transition_families(entries: list[dict[str, Any]], *, sample_limit: int) -> dict[str, Any]:
    """按 family 聚合 transition catalog，并嵌入阶段二机制审查。"""

    families: dict[str, Any] = {}
    for entry in entries:
        family = str(entry.get("family") or "unresolved_transition_mismatch")
        item = families.setdefault(
            family,
            {
                "family": family,
                "classification": entry.get("classification"),
                "status": entry.get("status"),
                "patch_risk": entry.get("patch_risk"),
                "prediction_count": 0,
                "exact_count": 0,
                "target_config_ids": [],
                "input_ids": [],
                "source_config_ids": [],
                "mismatch_kind_counts": {},
                "mismatch_totals_by_kind": {},
                "sample_pages": [],
                "sample_predictions": [],
                "mechanism_review": review_for_transition_family(family),
                "patch_filter_action": entry.get("patch_filter_action"),
                "source_attribution_required": entry.get("source_attribution_required"),
                "duration_required": entry.get("duration_required"),
                "evidence_required": entry.get("evidence_required"),
            },
        )
        item["prediction_count"] += 1
        item["exact_count"] += int(bool(entry.get("exact")))
        append_unique(item["target_config_ids"], entry.get("target_config_id"))
        append_unique(item["input_ids"], entry.get("input_id"))
        append_unique(item["source_config_ids"], entry.get("source_config_id"))
        for kind in entry.get("mismatch_kinds", []):
            increment_nested_count(item, "mismatch_kind_counts", str(kind), 1)
        merge_mismatch_totals(item["mismatch_totals_by_kind"], entry.get("mismatch_totals_by_kind", {}))
        for page in entry.get("sample_pages", []):
            append_unique(item["sample_pages"], page, limit=sample_limit)
        if len(item["sample_predictions"]) < sample_limit:
            item["sample_predictions"].append(
                {
                    "label": entry.get("label"),
                    "prediction_dir": entry.get("prediction_dir"),
                    "transition_exactness_path": entry.get("transition_exactness_path"),
                    "mismatch_kinds": entry.get("mismatch_kinds"),
                    "classification_reason": entry.get("classification_reason"),
                    "sample_mismatches": entry.get("sample_mismatches", [])[: min(5, sample_limit)],
                }
            )
    for item in families.values():
        item["target_config_ids"] = sorted(item["target_config_ids"])
        item["input_ids"] = sorted(item["input_ids"])
        item["source_config_ids"] = sorted(item["source_config_ids"])
        item["mismatch_kind_counts"] = dict(sorted(item["mismatch_kind_counts"].items()))
        item["mismatch_totals_by_kind"] = dict(sorted(item["mismatch_totals_by_kind"].items()))
    return dict(sorted(families.items()))


def review_for_transition_family(family: str) -> dict[str, Any]:
    """返回阶段二 family 机制审查模板。"""

    return dict(FAMILY_REVIEWS.get(family, FAMILY_REVIEWS["unresolved_transition_mismatch"]))


def dag_patch_gate_fields(classification: str, family: str) -> dict[str, Any]:
    """为 DAG patch 阶段生成最小 gate 字段。"""

    if classification == "matched":
        return {
            "patch_filter_action": "drop",
            "source_attribution_required": False,
            "duration_required": False,
            "evidence_required": [],
        }
    if classification == "state_marker_only":
        return {
            "patch_filter_action": "drop",
            "source_attribution_required": False,
            "duration_required": False,
            "evidence_required": ["transition family review"],
        }
    if classification == "transition_grouping":
        return {
            "patch_filter_action": "diagnostic_only",
            "source_attribution_required": False,
            "duration_required": False,
            "evidence_required": ["transition family review", "physical candidate promotion check"],
        }
    if classification == "async_visibility":
        return {
            "patch_filter_action": "diagnostic_only",
            "source_attribution_required": False,
            "duration_required": False,
            "evidence_required": ["checkpoint/control boundary evidence", "storage progress evidence"],
        }
    if classification == "capacity_victim":
        return {
            "patch_filter_action": "requires_source_attribution",
            "source_attribution_required": True,
            "duration_required": True,
            "evidence_required": ["capacity victim choices", "source cache physical ops", "duration calibration"],
        }
    if classification == "physical_candidate":
        return {
            "patch_filter_action": "requires_source_attribution",
            "source_attribution_required": True,
            "duration_required": True,
            "evidence_required": physical_candidate_evidence_required(family),
        }
    if classification == "model_bug":
        return {
            "patch_filter_action": "blocked",
            "source_attribution_required": False,
            "duration_required": False,
            "evidence_required": ["state model fix", "final-state regression check"],
        }
    return {
        "patch_filter_action": "blocked",
        "source_attribution_required": False,
        "duration_required": False,
        "evidence_required": ["observed oracle gap analysis"],
    }


def physical_candidate_evidence_required(family: str) -> list[str]:
    """按 high-risk family 给出 DAG patch 前置 evidence。"""

    if family == "writeback_eviction_interleaving":
        return [
            "writeback lifecycle grouping",
            "device eviction victim pages",
            "source cache physical ops",
            "duration calibration",
        ]
    if family == "low_host_cleanup_loadback_transient":
        return [
            "host cleanup victim choices",
            "loadback boundary or state fact",
            "prefetch apply evidence",
            "source cache physical ops",
            "duration calibration",
        ]
    return ["source cache physical ops", "duration calibration"]



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


def append_unique(values: list[Any], value: Any, *, limit: int | None = None) -> None:
    """向列表追加唯一值。"""

    if value is None or value == "":
        return
    if value in values:
        return
    if limit is not None and len(values) >= limit:
        return
    values.append(value)


def increment_nested_count(item: dict[str, Any], key: str, value: str, amount: int) -> None:
    """更新 dict 中的 counter-like 字段。"""

    counts = item.setdefault(key, {})
    counts[value] = int(counts.get(value, 0)) + amount


def merge_mismatch_totals(target: dict[str, Any], source: Any) -> None:
    """合并 mismatch_totals_by_kind。"""

    if not isinstance(source, dict):
        return
    for kind, value in source.items():
        if not isinstance(value, dict):
            continue
        item = target.setdefault(str(kind), {"mismatch_rows": 0, "missing_in_model": 0, "extra_in_model": 0})
        item["mismatch_rows"] += int(value.get("mismatch_rows") or 0)
        item["missing_in_model"] += int(value.get("missing_in_model") or 0)
        item["extra_in_model"] += int(value.get("extra_in_model") or 0)
