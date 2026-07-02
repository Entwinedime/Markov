"""transition family review 与 DAG patch gate 字段。"""

from __future__ import annotations

from typing import Any


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


def review_for_transition_family(family: str) -> dict[str, Any]:
    """返回阶段二 family 机制审查模板。"""

    return dict(FAMILY_REVIEWS.get(family, FAMILY_REVIEWS["unresolved_transition_mismatch"]))


def dag_patch_gate_fields(classification: str, family: str) -> dict[str, Any]:
    """为 DAG patch 阶段生成最小 gate 字段。"""

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
    if classification == "capacity_victim":
        return gate(
            "requires_source_attribution",
            True,
            True,
            ["capacity victim choices", "source cache physical ops", "duration calibration"],
        )
    if classification == "physical_candidate":
        return gate("requires_source_attribution", True, True, physical_candidate_evidence_required(family))
    if classification == "model_bug":
        return gate("blocked", False, False, ["state model fix", "final-state regression check"])
    return gate("blocked", False, False, ["observed oracle gap analysis"])


def gate(action: str, source_attribution: bool, duration: bool, evidence: list[str]) -> dict[str, Any]:
    """构造 patch gate 字段。"""

    return {
        "patch_filter_action": action,
        "source_attribution_required": source_attribution,
        "duration_required": duration,
        "evidence_required": evidence,
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
