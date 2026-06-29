"""HiCache workflow 矩阵的 profile quality gate。"""

from __future__ import annotations

import collections
import hashlib
from pathlib import Path
from typing import Any, Callable

from ..common.io import write_json
from ..common.trace import TraceLoadStatus, load_chrome_trace_events
from .facts import HICACHE_CONSUMER_STATE_MODEL, parse_fact_or_none
from .quality.profile_audit import audit_hicache_profile
from .input_contract_core import canonical_json, extract_audit_events, workload_identity_path_contract
from .matrix_types import ProfileRun, safe_slug


WORKLOAD_SIGNATURE_ROLES = (
    "request_admission",
    "request_bound_match_anchor",
    "request_lifecycle_anchor",
)


def build_quality_report(
    runs: list[ProfileRun],
    output_dir: Path,
    *,
    audit_dir: Path | None = None,
    summary_path: Path | None = None,
    on_row: Callable[[dict[str, Any]], None] | None = None,
) -> dict[str, Any]:
    """执行阶段一 workflow input gate，并写出 per-run HiCache audit。"""

    rows: list[dict[str, Any]] = []
    signature_by_input: dict[str, dict[str, Any]] = {}
    quality_dir = audit_dir or output_dir / "artifacts" / "quality"
    for run in runs:
        profile_audit_path = quality_dir / f"{safe_slug(run.run_id)}.hicache_profile_audit.json"
        profile_audit = audit_hicache_profile(run.manifest_path)
        write_json(profile_audit_path, profile_audit)
        trace_summary = summarize_profile_trace(run)
        workload_signature = build_workload_signature(run)
        forced_token_quality = normalize_forced_token_quality(profile_audit)
        state_ready = state_model_input_ready(profile_audit, trace_summary, workload_signature)
        row = {
            "run_id": run.run_id,
            "config_id": run.config_id,
            "input_id": run.input_id,
            "input_class": run.input_class,
            "manifest_path": str(run.manifest_path),
            "run_dir": str(run.run_dir),
            "hicache_profile_audit_path": str(profile_audit_path),
            "python_probe_files": [str(path) for path in run.python_probe_files],
            "python_probe_file_count": len(run.python_probe_files),
            "trace_load_status": trace_summary["trace_load_status"],
            "workload_event_count": trace_summary["fact_class_counts"].get("workload_identity", 0),
            "hicache_state_model_event_count": trace_summary["consumer_counts"].get(HICACHE_CONSUMER_STATE_MODEL, 0),
            "oracle_snapshot_count": trace_summary["oracle_snapshot_count"],
            "source_actual_count": trace_summary["fact_class_counts"].get("source_actual", 0),
            "timing_count": trace_summary["fact_class_counts"].get("timing_observation", 0),
            "fact_class_counts": trace_summary["fact_class_counts"],
            "fact_role_counts": trace_summary["fact_role_counts"],
            "consumer_counts": trace_summary["consumer_counts"],
            "missing_required_fields": profile_audit.get("hicache_state_model_fact_coverage", {}).get(
                "missing_fields",
                {},
            ),
            "canonical_request_count": workload_signature["request_event_count"],
            "canonical_workload_signature": workload_signature["signature"],
            "canonical_workload_ready": workload_signature["ready"],
            "artifact_ready": bool(profile_audit.get("artifact_ready")),
            "artifact_errors": profile_audit.get("artifact_errors", []),
            "strict_diagnostic_coverage_ready": bool(profile_audit.get("strict_diagnostic_coverage_ready")),
            "diagnostic_coverage_errors": profile_audit.get("diagnostic_coverage_errors", []),
            "state_model_input_ready": state_ready,
            "state_model_input_errors": profile_audit.get("state_model_input_errors", []),
            "workflow_input_ready": state_ready,
            "workflow_input_errors": profile_audit.get("workflow_input_errors", []),
            "forced_token_enabled": bool(forced_token_quality.get("enabled")),
            "forced_token_ready": bool(forced_token_quality.get("ready")),
            "forced_token_plan_ready": bool(forced_token_quality.get("plan_ready")),
            "forced_token_bundle_ready": bool(forced_token_quality.get("bundle_ready")),
            "forced_token_mode": forced_token_quality.get("mode"),
            "forced_token_plan_sha256": forced_token_quality.get("plan_sha256"),
            "forced_token_bundle_sha256": forced_token_quality.get("bundle_sha256"),
            "forced_token_bundle_id": forced_token_quality.get("bundle_id"),
            "forced_token_bundle_path": forced_token_quality.get("bundle_path"),
            "forced_token_quality": forced_token_quality,
            "hicache_state_model_fact_coverage": profile_audit.get("hicache_state_model_fact_coverage", {}),
            "hicache_capacity": profile_audit.get("hicache_capacity", {}),
            "workload_signature_detail": workload_signature,
        }
        rows.append(row)
        if on_row is not None:
            on_row(row)

    by_input: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
    for row in rows:
        by_input[str(row["input_id"])].append(row)
    for input_id, input_rows in sorted(by_input.items()):
        signatures = sorted(
            {
                str(row["canonical_workload_signature"])
                for row in input_rows
                if row["canonical_workload_signature"]
            }
        )
        signature_by_input[input_id] = {
            "input_id": input_id,
            "run_count": len(input_rows),
            "config_ids": sorted(str(row["config_id"]) for row in input_rows),
            "signature_count": len(signatures),
            "signature_match": len(signatures) == 1,
            "signatures": signatures,
            **summarize_forced_token_input_group(input_rows),
        }
        signature_by_input[input_id]["input_contract_ready"] = (
            signature_by_input[input_id]["signature_match"]
            and signature_by_input[input_id]["forced_token_enabled_count"] == signature_by_input[input_id]["run_count"]
            and signature_by_input[input_id]["forced_token_plan_signature_match"]
            and signature_by_input[input_id]["forced_token_bundle_signature_match"]
        )

    workflow_input_ready = (
        all(row["state_model_input_ready"] for row in rows)
        and all(row["signature_match"] for row in signature_by_input.values())
        and all(row["forced_token_plan_signature_match"] for row in signature_by_input.values())
        and all(row["forced_token_bundle_signature_match"] for row in signature_by_input.values())
    )
    report = {
        "schema": "trace_sim.hicache.state_matrix.workflow_input_quality.v1",
        "stage": "quality",
        "run_count": len(rows),
        "config_ids": sorted({run.config_id for run in runs}),
        "input_ids": sorted({run.input_id for run in runs}),
        "workflow_input_ready": workflow_input_ready,
        "workflow_input_ready_count": sum(1 for row in rows if row["workflow_input_ready"]),
        "state_model_input_ready_count": sum(1 for row in rows if row["state_model_input_ready"]),
        "strict_diagnostic_coverage_ready_count": sum(
            1 for row in rows if row["strict_diagnostic_coverage_ready"]
        ),
        "artifact_ready_count": sum(1 for row in rows if row["artifact_ready"]),
        "input_workload_signatures": signature_by_input,
        "runs": rows,
        "note": (
            "workflow_input_ready gates modeling workflow execution. "
            "state_model_input_ready covers state facts, token dictionary/span, oracle snapshots, "
            "workload identity, and forced-token readiness. strict_diagnostic_coverage_ready is reported "
            "separately and does not block the default workflow gate."
        ),
    }
    write_json(summary_path or output_dir / "stages" / "quality" / "summary.json", report)
    return report


def normalize_forced_token_quality(profile_audit: dict[str, Any]) -> dict[str, Any]:
    """从 HiCache profile audit 中提取稳定的 forced-token gate 摘要。"""

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
    """汇总同一个 input 下的 forced-token plan/bundle 一致性。"""

    forced_rows = [row for row in input_rows if row.get("forced_token_enabled")]
    plan_signatures = sorted(
        {
            str(row.get("forced_token_plan_sha256"))
            for row in forced_rows
            if row.get("forced_token_plan_sha256")
        }
    )
    signature_match = (
        not forced_rows
        or (
            len(forced_rows) == len(input_rows)
            and len(plan_signatures) == 1
            and all(row.get("forced_token_plan_ready") for row in forced_rows)
        )
    )
    bundle_signatures = sorted(
        {
            str(row.get("forced_token_bundle_sha256"))
            for row in forced_rows
            if row.get("forced_token_bundle_sha256")
        }
    )
    bundle_ids = sorted(
        {
            str(row.get("forced_token_bundle_id"))
            for row in forced_rows
            if row.get("forced_token_bundle_id")
        }
    )
    bundle_signature_match = (
        not forced_rows
        or (
            len(forced_rows) == len(input_rows)
            and len(bundle_signatures) == 1
            and len(bundle_ids) == 1
            and all(row.get("forced_token_bundle_ready") for row in forced_rows)
        )
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


def summarize_profile_trace(run: ProfileRun) -> dict[str, Any]:
    """汇总一个 profile 的 fact class、role、consumer 和 oracle snapshot 覆盖情况。"""

    fact_class_counts: collections.Counter[str] = collections.Counter()
    fact_role_counts: collections.Counter[str] = collections.Counter()
    consumer_counts: collections.Counter[str] = collections.Counter()
    oracle_snapshot_count = 0
    statuses: list[TraceLoadStatus] = []
    for path in run.python_probe_files:
        events, status = load_chrome_trace_events(path, auto_repair=True)
        statuses.append(status)
        for event in events:
            args = event.get("args") if isinstance(event.get("args"), dict) else {}
            fact = parse_fact_or_none(args)
            if fact is not None:
                fact_class_counts[fact.fact_class] += 1
                fact_role_counts[fact.role] += 1
                for consumer in fact.consumers:
                    consumer_counts[consumer] += 1
            if fact is not None and fact.fact_class == "oracle_state" and fact.role == "state_snapshot":
                oracle_snapshot_count += 1
    return {
        "fact_class_counts": dict(sorted(fact_class_counts.items())),
        "fact_role_counts": dict(sorted(fact_role_counts.items())),
        "consumer_counts": dict(sorted(consumer_counts.items())),
        "oracle_snapshot_count": oracle_snapshot_count,
        "trace_load_status": [status.to_dict() for status in statuses],
    }


def build_workload_signature(run: ProfileRun) -> dict[str, Any]:
    """基于 workload identity fact 构造同 input 可比较的 workload 签名。"""

    roles = set(WORKLOAD_SIGNATURE_ROLES)
    events, unknown_roles, unmapped_requests = extract_audit_events(list(run.python_probe_files), run.run_id, roles)
    path_contract = workload_identity_path_contract(list(run.python_probe_files), roles, sample=0)
    by_role: dict[str, collections.Counter[str]] = {role: collections.Counter() for role in roles}
    for event in events:
        by_role.setdefault(event.role, collections.Counter())[event.signature] += 1
    payload = {
        "roles": {
            role: [
                {"signature": signature, "count": count}
                for signature, count in sorted(counter.items())
            ]
            for role, counter in sorted(by_role.items())
        }
    }
    encoded = canonical_json(payload)
    signature = "sha256_json:" + hashlib.sha256(encoded.encode("utf-8")).hexdigest()
    unknown = dict(sorted(unknown_roles.items()))
    unmapped = dict(sorted(unmapped_requests.items()))
    return {
        "signature": signature,
        "ready": bool(events) and not unknown and not unmapped and bool(path_contract.get("ready")),
        "request_event_count": len(events),
        "roles": sorted(roles),
        "role_counts": {role: sum(counter.values()) for role, counter in sorted(by_role.items())},
        "unknown_workload_identity_roles": unknown,
        "unmapped_request_id_events": unmapped,
        "workload_identity_path_contract": path_contract,
    }


def state_model_input_ready(
    profile_audit: dict[str, Any],
    trace_summary: dict[str, Any],
    workload_signature: dict[str, Any],
) -> bool:
    """判断 profile 是否满足 final-state 建模输入要求。"""

    if profile_audit.get("state_model_input_ready") is not True:
        return False
    state_model_fact = profile_audit.get("hicache_state_model_fact_coverage")
    if not isinstance(state_model_fact, dict) or not state_model_fact.get("ready", False):
        return False
    if trace_summary.get("oracle_snapshot_count", 0) <= 0:
        return False
    if trace_summary.get("consumer_counts", {}).get(HICACHE_CONSUMER_STATE_MODEL, 0) <= 0:
        return False
    forced_token_quality = profile_audit.get("forced_token_quality")
    if isinstance(forced_token_quality, dict) and forced_token_quality.get("enabled"):
        if not forced_token_quality.get("ready"):
            return False
    return bool(workload_signature.get("ready"))
