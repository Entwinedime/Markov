"""Transition-family reviews and diagnostic DAG-patch gate fields."""

from __future__ import annotations

from typing import Any


def family_review(
    status: str,
    patch_risk: str,
    classification: str,
    observed_evidence: str,
    recommended_fix: str,
) -> dict[str, Any]:
    """Build the compact family review embedded in the catalog."""

    return {
        "status": status,
        "patch_risk": patch_risk,
        "classification": classification,
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
    "host_cleanup_loadback_visibility": family_review(
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


def review_for_transition_family(family: str) -> dict[str, Any]:
    """Return the mechanism-review template for one transition family."""

    return dict(FAMILY_REVIEWS.get(family, FAMILY_REVIEWS["unresolved_transition_mismatch"]))


def dag_patch_gate_fields(classification: str, family: str) -> dict[str, Any]:
    """Build minimum diagnostic gate fields for future DAG patching."""

    if classification == "matched":
        return gate("drop", False, False, [])
    if classification == "state_marker_only":
        return gate("drop", False, False, ["transition family review"])
    if classification == "transition_grouping":
        return gate("diagnostic_only", False, False, ["transition family review", "physical candidate promotion check"])
    if classification == "async_visibility":
        return gate(
            "diagnostic_only", False, False, ["checkpoint/control boundary evidence", "storage progress evidence"]
        )
    if classification == "physical_candidate":
        return gate("requires_source_attribution", True, True, physical_candidate_evidence_required(family))
    return gate("blocked", False, False, ["observed oracle gap analysis"])


def gate(action: str, source_attribution: bool, duration: bool, evidence: list[str]) -> dict[str, Any]:
    """Construct one patch-gate decision payload."""

    return {
        "patch_filter_action": action,
        "source_attribution_required": source_attribution,
        "duration_required": duration,
        "evidence_required": evidence,
    }


def physical_candidate_evidence_required(family: str) -> list[str]:
    """Return source evidence required by a high-risk physical family."""

    if family == "writeback_eviction_interleaving":
        return [
            "writeback lifecycle grouping",
            "device eviction victim pages",
            "source cache physical ops",
            "duration calibration",
        ]
    if family == "host_cleanup_loadback_visibility":
        return [
            "host cleanup victim choices",
            "loadback boundary or state fact",
            "prefetch apply evidence",
            "source cache physical ops",
            "duration calibration",
        ]
    return ["source cache physical ops", "duration calibration"]
