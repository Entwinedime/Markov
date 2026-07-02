"""HiCache state input preflight 的 ready gate 辅助工具。"""

from __future__ import annotations

from typing import Any


def public_preflight_row(row: dict[str, Any]) -> dict[str, Any]:
    """移除 report 内部临时字段。"""

    return {key: value for key, value in row.items() if not str(key).startswith("_")}


def normalize_forced_token_preflight(profile_audit: dict[str, Any]) -> dict[str, Any]:
    """从 HiCache profile audit 中提取稳定 forced-token gate 字段。"""

    quality = profile_audit.get("forced_token_quality")
    if not isinstance(quality, dict):
        return {"enabled": False, "ready": True, "mode": "none", "plan_sha256": None}
    return {
        **quality,
        "enabled": bool(quality.get("enabled")),
        "ready": bool(quality.get("ready")),
        "mode": quality.get("mode") or "none",
        "plan_sha256": quality.get("plan_sha256"),
    }


def summarize_forced_token_input_group(input_rows: list[dict[str, Any]]) -> dict[str, Any]:
    """汇总同一个 input 下 forced-token plan/bundle 的跨 config 一致性。"""

    forced_rows = [row for row in input_rows if row.get("forced_token_enabled")]
    plan_signatures = sorted(
        {str(row.get("forced_token_plan_sha256")) for row in forced_rows if row.get("forced_token_plan_sha256")}
    )
    signature_match = not forced_rows or (
        len(forced_rows) == len(input_rows)
        and len(plan_signatures) == 1
        and all(row.get("forced_token_plan_ready") for row in forced_rows)
    )
    bundle_signatures = sorted(
        {str(row.get("forced_token_bundle_sha256")) for row in forced_rows if row.get("forced_token_bundle_sha256")}
    )
    bundle_ids = sorted(
        {str(row.get("forced_token_bundle_id")) for row in forced_rows if row.get("forced_token_bundle_id")}
    )
    bundle_signature_match = not forced_rows or (
        len(forced_rows) == len(input_rows)
        and len(bundle_signatures) == 1
        and len(bundle_ids) == 1
        and all(row.get("forced_token_bundle_ready") for row in forced_rows)
    )
    return {
        "forced_token_enabled_count": len(forced_rows),
        "forced_token_plan_signature_count": len(plan_signatures),
        "forced_token_plan_signature_match": signature_match,
        "forced_token_plan_signatures": plan_signatures,
        "forced_token_bundle_signature_count": len(bundle_signatures),
        "forced_token_bundle_signature_match": bundle_signature_match,
        "forced_token_bundle_signatures": bundle_signatures,
        "forced_token_bundle_ids": bundle_ids,
    }


def state_model_input_ready(
    profile_audit: dict[str, Any],
    workload_signature: dict[str, Any],
) -> bool:
    """判断 profile 是否满足 state-model workload identity 输入要求。"""

    if profile_audit.get("state_model_input_ready") is not True:
        return False
    state_model_fact = profile_audit.get("hicache_state_model_fact_coverage")
    if not isinstance(state_model_fact, dict) or not state_model_fact.get("ready", False):
        return False
    if state_model_fact.get("hicache_state_model_event_count", 0) <= 0:
        return False
    return bool(workload_signature.get("ready"))


def workflow_input_ready(
    profile_audit: dict[str, Any],
    state_ready: bool,
    *,
    require_validation_evidence: bool,
) -> bool:
    """判断选中的 validation workflow 是否可以继续。"""

    if not state_ready:
        return False
    if not require_validation_evidence:
        return True
    return profile_audit.get("validator_evidence_ready") is True
