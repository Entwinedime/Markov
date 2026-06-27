"""Profile quality gate for HiCache workflow matrices."""

from __future__ import annotations

import collections
import hashlib
import json
from pathlib import Path
from typing import Any

from ..common.io import write_json
from ..common.trace import TraceLoadStatus, load_chrome_trace_events
from ..profiling.quality import audit_profile
from .facts import HICACHE_CONSUMER_STATE_MODEL, parse_fact_or_none
from .input_contract_core import canonical_json, extract_audit_events, workload_identity_path_contract
from .matrix_types import ProfileRun, safe_slug


WORKLOAD_SIGNATURE_ROLES = (
    "request_admission",
    "request_bound_match_anchor",
    "request_lifecycle_anchor",
)


def build_quality_report(runs: list[ProfileRun], output_dir: Path, *, progress: bool = False) -> dict[str, Any]:
    """执行阶段一 profile quality gate，并写出 per-run 质量详情。"""

    rows: list[dict[str, Any]] = []
    signature_by_input: dict[str, dict[str, Any]] = {}
    quality_dir = output_dir / "quality"
    total = len(runs)
    for index, run in enumerate(runs, start=1):
        if progress:
            print(f"[{index}/{total}] run {run.input_id}/{run.config_id}", flush=True)
        profile_quality_path = quality_dir / f"{safe_slug(run.run_id)}.profile_quality.json"
        profile_quality = audit_profile(run.manifest_path)
        write_json(profile_quality_path, profile_quality)
        trace_summary = summarize_profile_trace(run)
        workload_signature = build_workload_signature(run)
        forced_token_quality = normalize_forced_token_quality(profile_quality)
        row = {
            "run_id": run.run_id,
            "config_id": run.config_id,
            "input_id": run.input_id,
            "input_class": run.input_class,
            "manifest_path": str(run.manifest_path),
            "run_dir": str(run.run_dir),
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
            "missing_required_fields": profile_quality.get("hicache_state_model_fact_coverage", {}).get(
                "missing_fields",
                {},
            ),
            "canonical_request_count": workload_signature["request_event_count"],
            "canonical_workload_signature": workload_signature["signature"],
            "canonical_workload_ready": workload_signature["ready"],
            "profile_quality_ready": bool(profile_quality.get("quality_ready")),
            "profile_quality_errors": profile_quality.get("quality_errors", []),
            "state_quality_ready": state_quality_ready(profile_quality, trace_summary, workload_signature),
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
            "hicache_state_model_fact_coverage": profile_quality.get("hicache_state_model_fact_coverage", {}),
            "hicache_capacity": profile_quality.get("hicache_capacity", {}),
            "workload_signature_detail": workload_signature,
        }
        rows.append(row)
        if progress:
            print_quality_result(index, total, row)

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

    quality_ready = (
        all(row["state_quality_ready"] for row in rows)
        and all(row["signature_match"] for row in signature_by_input.values())
        and all(row["forced_token_plan_signature_match"] for row in signature_by_input.values())
        and all(row["forced_token_bundle_signature_match"] for row in signature_by_input.values())
    )
    report = {
        "schema": "trace_sim.hicache.state_matrix.profile_quality.v1",
        "stage": "quality",
        "run_count": len(rows),
        "config_ids": sorted({run.config_id for run in runs}),
        "input_ids": sorted({run.input_id for run in runs}),
        "quality_ready": quality_ready,
        "state_quality_ready_count": sum(1 for row in rows if row["state_quality_ready"]),
        "profile_quality_ready_count": sum(1 for row in rows if row["profile_quality_ready"]),
        "input_workload_signatures": signature_by_input,
        "runs": rows,
        "note": (
            "quality_ready uses state modeling requirements: state model facts, token dictionary/span, "
            "oracle snapshots, same-input workload signatures, and forced-token plan/bundle consistency "
            "when forced replay is enabled. "
            "profile_quality_ready keeps the stricter collection audit for source_actual/timing diagnostics."
        ),
    }
    write_json(output_dir / "profile_quality.json", report)
    return report


def print_quality_result(index: int, total: int, row: dict[str, Any]) -> None:
    """打印 profile quality 的简短结果行。"""

    status = "ok" if row.get("state_quality_ready") is True else "fail"
    issue_text = quality_issue_text(row)
    print(
        f"[{index}/{total}] result {status} "
        f"state_ready={progress_value(row.get('state_quality_ready'))} "
        f"profile_ready={progress_value(row.get('profile_quality_ready'))} "
        f"workload_events={row.get('canonical_request_count')} "
        f"oracle_snapshots={row.get('oracle_snapshot_count')} "
        f"state_model_events={row.get('hicache_state_model_event_count')}"
        f"{issue_text}",
        flush=True,
    )


def quality_issue_text(row: dict[str, Any]) -> str:
    """返回 quality result 行中的短问题摘要。"""

    errors = [str(item) for item in row.get("profile_quality_errors", []) if item]
    coverage = row.get("hicache_state_model_fact_coverage")
    invalid_count = 0
    if isinstance(coverage, dict):
        invalid_count = int(coverage.get("invalid_token_dictionary_issue_count") or 0)
    parts = []
    if errors:
        parts.append("issues=" + ",".join(errors[:3]))
    if invalid_count:
        parts.append(f"token_dict_invalid={invalid_count}")
    return (" " + " ".join(parts)) if parts else ""


def progress_value(value: Any) -> str:
    """把进度行中的 Python 值转成短字符串。"""

    if isinstance(value, bool) or value is None:
        return json.dumps(value)
    return str(value)


def normalize_forced_token_quality(profile_quality: dict[str, Any]) -> dict[str, Any]:
    """从 profile_quality 中提取稳定的 forced-token gate 摘要。"""

    quality = profile_quality.get("forced_token_quality")
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


def state_quality_ready(
    profile_quality: dict[str, Any],
    trace_summary: dict[str, Any],
    workload_signature: dict[str, Any],
) -> bool:
    """判断 profile 是否满足 final-state 建模输入要求。"""

    state_model_fact = profile_quality.get("hicache_state_model_fact_coverage")
    if not isinstance(state_model_fact, dict) or not state_model_fact.get("ready", False):
        return False
    if trace_summary.get("oracle_snapshot_count", 0) <= 0:
        return False
    if trace_summary.get("consumer_counts", {}).get(HICACHE_CONSUMER_STATE_MODEL, 0) <= 0:
        return False
    forced_token_quality = profile_quality.get("forced_token_quality")
    if isinstance(forced_token_quality, dict) and forced_token_quality.get("enabled"):
        if not forced_token_quality.get("ready"):
            return False
    return bool(workload_signature.get("ready"))
