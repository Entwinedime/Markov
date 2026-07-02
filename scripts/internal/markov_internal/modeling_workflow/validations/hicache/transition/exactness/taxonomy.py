"""HiCache transition family 分类。"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from .taxonomy_constants import (
    DEVICE_VISIBLE_DELTA_KINDS,
    HOST_VISIBLE_DELTA_KINDS,
    MARKER_DELTA_KINDS,
    PREFETCH_DELTA_KINDS,
)
from .taxonomy_evidence import summarize_hicache_evidence, summarize_observed_target_evidence
from .taxonomy_reviews import dag_patch_gate_fields, review_for_transition_family


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
        {str(item.get("page")) for item in sample_mismatches if isinstance(item, dict) and item.get("page") is not None}
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
    prediction_row: dict[str, Any],
    comparison: dict[str, Any],
    hicache_summary: dict[str, Any],
    mismatch_kinds: list[str],
) -> tuple[str, str]:
    """把 transition mismatch kind 组合规整成阶段一 family。"""

    del hicache_summary
    target_config = str(prediction_row.get("target_config_id") or comparison.get("target_config_id") or "")
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
    if target_config.startswith("c2_") and kind_set <= {"mark_dirty", "clear_dirty", "mark_evicted", "clear_evicted"}:
        return "writeback_eviction_interleaving", "write-back target dirty/evicted lifecycle mismatch"
    if target_config.startswith("c1_"):
        if kind_set <= {"mark_dirty", "clear_dirty"}:
            return "dirty_oscillation", "write-through-selective target dirty marker lifecycle mismatch"
        if kind_set <= {"mark_dirty", "clear_dirty", "mark_evicted", "clear_evicted"}:
            return (
                "dirty_evicted_marker_oscillation",
                "write-through-selective dirty marker also shifts evicted marker boundary",
            )
    if kind_set <= {"mark_evicted", "clear_evicted"}:
        return "evicted_marker_oscillation", "only evicted marker lifecycle differs"
    if kind_set <= {"mark_dirty", "clear_dirty"}:
        return "dirty_oscillation", "only dirty marker lifecycle differs"
    if kind_set <= MARKER_DELTA_KINDS:
        return (
            "dirty_evicted_marker_oscillation",
            "only marker deltas differ, but kind combination is not target-specific",
        )
    return "unresolved_transition_mismatch", "no family rule matched this transition mismatch shape"
