"""Semantic family classification for HiCache transition mismatches."""

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


TransitionClassification = tuple[str, str]


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
    """Project one comparison to a transition family and patch gate."""

    mismatch_kinds = transition_mismatch_kinds(comparison)
    family, reason = classify_transition_family(comparison, hicache_summary, mismatch_kinds)
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
    """Extract compact classification and gate fields for comparison output."""

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
    """Collect transition mismatch kinds from one comparison cell."""

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
    comparison: dict[str, Any],
    hicache_summary: dict[str, Any],
    mismatch_kinds: list[str],
) -> tuple[str, str]:
    """Classify a mismatch from semantic deltas and resolved target policy.

    Configuration identifiers are deliberately excluded: names such as ``c2``
    are experiment labels, not model parameters, and cannot justify a mechanism
    classification.
    """

    resolved_policy = (
        hicache_summary.get("resolved_policy") if isinstance(hicache_summary.get("resolved_policy"), dict) else {}
    )
    target_config = (
        hicache_summary.get("target_config") if isinstance(hicache_summary.get("target_config"), dict) else {}
    )
    write_policy = str(resolved_policy.get("write_policy") or target_config.get("write_policy") or "")
    kind_set = set(mismatch_kinds)
    for classification in (
        _hard_gate_classification(comparison, kind_set),
        _visibility_classification(kind_set),
        _policy_classification(write_policy, kind_set),
        _marker_classification(kind_set),
    ):
        if classification is not None:
            return classification
    return "unresolved_transition_mismatch", "no family rule matched this transition mismatch shape"


def _hard_gate_classification(comparison: dict[str, Any], kind_set: set[str]) -> TransitionClassification | None:
    """Apply readiness, final-state, and exactness gates before mechanism rules."""

    if not comparison or not comparison.get("ready"):
        return "model_or_oracle_not_ready", "transition exactness output is missing or not ready"
    if not comparison.get("final_state_exact"):
        return "model_or_oracle_not_ready", "final-state hard gate is not exact; transition catalog must not continue"
    if comparison.get("exact") and not kind_set:
        return "transition_exact", "final state, transition count and page lifecycle are exact"
    return None


def _visibility_classification(kind_set: set[str]) -> TransitionClassification | None:
    """Classify prefetch and cross-tier visibility mismatches before policy markers."""

    if kind_set & PREFETCH_DELTA_KINDS:
        return "prefetch_visibility", "prefetch visible state delta mismatch"
    if kind_set & HOST_VISIBLE_DELTA_KINDS and kind_set & DEVICE_VISIBLE_DELTA_KINDS:
        return (
            "host_cleanup_loadback_visibility",
            "host-visible and device-visible residency deltas diverge in the same prediction",
        )
    return None


def _policy_classification(write_policy: str, kind_set: set[str]) -> TransitionClassification | None:
    """Apply target write-policy rules to marker-only mismatch shapes."""

    if write_policy == "write_back" and kind_set <= {
        "mark_dirty",
        "clear_dirty",
        "mark_evicted",
        "clear_evicted",
    }:
        return "writeback_eviction_interleaving", "write-back target dirty/evicted lifecycle mismatch"
    if write_policy == "write_through_selective":
        if kind_set <= {"mark_dirty", "clear_dirty"}:
            return "dirty_oscillation", "write-through-selective target dirty marker lifecycle mismatch"
        if kind_set <= {"mark_dirty", "clear_dirty", "mark_evicted", "clear_evicted"}:
            return (
                "dirty_evicted_marker_oscillation",
                "write-through-selective dirty marker also shifts evicted marker boundary",
            )
    return None


def _marker_classification(kind_set: set[str]) -> TransitionClassification | None:
    """Classify policy-independent marker-only mismatch shapes."""

    if kind_set <= {"mark_evicted", "clear_evicted"}:
        return "evicted_marker_oscillation", "only evicted marker lifecycle differs"
    if kind_set <= {"mark_dirty", "clear_dirty"}:
        return "dirty_oscillation", "only dirty marker lifecycle differs"
    if kind_set <= MARKER_DELTA_KINDS:
        return (
            "dirty_evicted_marker_oscillation",
            "only marker deltas differ, but kind combination is not target-specific",
        )
    return None
