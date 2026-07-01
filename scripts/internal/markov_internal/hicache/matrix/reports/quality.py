"""HiCache workflow 矩阵的 profile quality gate。"""

from __future__ import annotations

import collections
import hashlib
from pathlib import Path
from typing import Any, Callable

from ....common.io import write_json
from ...input_contract.signature.core import canonical_json, extract_audit_events, workload_identity_path_contract
from ...quality.audit.profile import HiCacheProfileAuditOptions, audit_hicache_profile
from ..runs.types import ProfileRun, safe_slug


WORKLOAD_SIGNATURE_ROLES = (
    "cache_lookup_input",
    "cache_extend_input",
    "cache_lifecycle_commit",
    "prefetch_candidate_anchor",
)


def build_quality_report(
    runs: list[ProfileRun],
    output_dir: Path,
    *,
    audit_dir: Path | None = None,
    summary_path: Path | None = None,
    on_row: Callable[[dict[str, Any]], None] | None = None,
    require_validation_evidence: bool = False,
    validate_diagnostic_coverage: bool = False,
    require_cross_config_contract: bool = False,
    show_workload_sequence: bool = False,
) -> dict[str, Any]:
    """执行阶段一 workflow input gate，并写出 per-run HiCache audit。"""

    rows: list[dict[str, Any]] = []
    signature_by_input: dict[str, dict[str, Any]] = {}
    quality_dir = audit_dir or output_dir / "artifacts" / "quality"
    audit_options = HiCacheProfileAuditOptions(
        validate_forced_token=require_cross_config_contract,
        validate_oracle_evidence=require_validation_evidence,
        validate_diagnostic_coverage=validate_diagnostic_coverage,
    )
    for run in runs:
        profile_audit_path = quality_dir / f"{safe_slug(run.run_id)}.hicache_profile_audit.json"
        profile_audit = audit_hicache_profile(run.manifest_path, options=audit_options)
        write_json(profile_audit_path, profile_audit)
        workload_signature = build_workload_signature(
            run,
            include_sequence_events=show_workload_sequence,
        )
        forced_token_quality = normalize_forced_token_quality(profile_audit) if require_cross_config_contract else None
        state_ready = state_model_input_ready(profile_audit, workload_signature)
        workflow_ready = workflow_input_ready(
            profile_audit,
            state_ready,
            require_validation_evidence=require_validation_evidence,
        )
        row = {
            "run_id": run.run_id,
            "config_id": run.config_id,
            "input_id": run.input_id,
            "input_class": run.input_class,
            "manifest_path": str(run.manifest_path),
            "run_dir": str(run.run_dir),
            "hicache_profile_audit_path": str(profile_audit_path),
            "python_probe_file_count": len(run.python_probe_files),
            "requested_consumers": profile_audit.get("requested_consumers", []),
            "canonical_request_count": workload_signature["request_event_count"],
            "canonical_workload_signature": workload_signature["signature"],
            "canonical_workload_ready": workload_signature["ready"],
            "canonical_workload_sequence_signature": workload_signature["sequence_signature"],
            "canonical_workload_sequence_ready": workload_signature["sequence_ready"],
            "artifact_ready": bool(profile_audit.get("artifact_ready")),
            "artifact_errors": profile_audit.get("artifact_errors", []),
            "state_model_input_ready": state_ready,
            "state_model_input_errors": profile_audit.get("state_model_input_errors", []),
            "workflow_input_ready": workflow_ready,
            "workflow_input_errors": profile_audit.get("workflow_input_errors", []),
        }
        if show_workload_sequence:
            row["_workload_sequence_events"] = workload_signature.get("sequence_events", [])
        if validate_diagnostic_coverage:
            row.update(
                {
                    "strict_diagnostic_coverage_ready": profile_audit.get("strict_diagnostic_coverage_ready"),
                    "diagnostic_coverage_errors": profile_audit.get("diagnostic_coverage_errors", []),
                }
            )
        if require_validation_evidence:
            row.update(
                {
                    "validator_evidence_ready": profile_audit.get("validator_evidence_ready"),
                    "validator_evidence_errors": profile_audit.get("validator_evidence_errors", []),
                }
            )
        if forced_token_quality is not None:
            row.update(
                {
                    "forced_token_enabled": bool(forced_token_quality.get("enabled")),
                    "forced_token_ready": bool(forced_token_quality.get("ready")),
                    "forced_token_plan_ready": bool(forced_token_quality.get("plan_ready")),
                    "forced_token_bundle_ready": bool(forced_token_quality.get("bundle_ready")),
                    "forced_token_mode": forced_token_quality.get("mode"),
                    "forced_token_plan_sha256": forced_token_quality.get("plan_sha256"),
                    "forced_token_bundle_sha256": forced_token_quality.get("bundle_sha256"),
                    "forced_token_bundle_id": forced_token_quality.get("bundle_id"),
                    "forced_token_bundle_path": forced_token_quality.get("bundle_path"),
                }
            )
        rows.append(row)
        if on_row is not None:
            on_row(row)

    by_input: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
    for row in rows:
        by_input[str(row["input_id"])].append(row)
    for input_id, input_rows in sorted(by_input.items()):
        signatures = sorted(
            {str(row["canonical_workload_signature"]) for row in input_rows if row["canonical_workload_signature"]}
        )
        forced_token_summary = summarize_forced_token_input_group(input_rows) if require_cross_config_contract else {}
        sequence_summary = summarize_workload_sequence_input_group(
            input_rows,
            include_details=show_workload_sequence,
        )
        signature_match = len(signatures) == 1 if require_cross_config_contract else None
        signature_by_input[input_id] = {
            "input_id": input_id,
            "run_count": len(input_rows),
            "config_ids": sorted(str(row["config_id"]) for row in input_rows),
            "signature_count": len(signatures),
            "signature_match": signature_match,
            "signatures": signatures,
            "canonical_workload_ready": all(row.get("canonical_workload_ready") for row in input_rows),
            **sequence_summary,
            **forced_token_summary,
        }
        input_ready = bool(signature_by_input[input_id]["canonical_workload_ready"])
        if require_cross_config_contract:
            input_ready = input_ready and (
                signature_by_input[input_id]["signature_match"] is True
                and signature_by_input[input_id]["forced_token_enabled_count"]
                == signature_by_input[input_id]["run_count"]
                and signature_by_input[input_id]["forced_token_plan_signature_match"]
                and signature_by_input[input_id]["forced_token_bundle_signature_match"]
            )
        signature_by_input[input_id]["input_contract_ready"] = input_ready

    all_workflow_inputs_ready = all(row["workflow_input_ready"] for row in rows) and all(
        row["input_contract_ready"] for row in signature_by_input.values()
    )
    report = {
        "schema": "trace_sim.hicache.state_matrix.workflow_input_quality.v1",
        "stage": "quality",
        "quality_options": {
            "require_validation_evidence": require_validation_evidence,
            "validate_diagnostic_coverage": validate_diagnostic_coverage,
            "require_cross_config_contract": require_cross_config_contract,
            "show_workload_sequence": show_workload_sequence,
        },
        "run_count": len(rows),
        "config_ids": sorted({run.config_id for run in runs}),
        "input_ids": sorted({run.input_id for run in runs}),
        "workflow_input_ready": all_workflow_inputs_ready,
        "workflow_input_ready_count": sum(1 for row in rows if row["workflow_input_ready"]),
        "state_model_input_ready_count": sum(1 for row in rows if row["state_model_input_ready"]),
        "artifact_ready_count": sum(1 for row in rows if row["artifact_ready"]),
        "sequence_check_display_enabled": show_workload_sequence,
        "input_sequence_match_count": sum(
            1 for item in signature_by_input.values() if item.get("sequence_match") is True
        ),
        "input_workload_signatures": signature_by_input,
        "runs": [public_quality_row(row) for row in rows],
        "note": (
            "workflow_input_ready gates modeling workflow execution. "
            "state_model_input_ready covers only workload identity state facts, token dictionary/span, "
            "and workload identity signatures. workflow_input_ready additionally requires validation evidence "
            "and forced-token cross-config checks only when this workflow asks for them. "
            "Validation-only fields are omitted when their execution path is disabled. "
            "Canonical workload signatures compare content multisets; sequence order is a separate diagnostic "
            "check and does not gate the cross-config input contract."
        ),
    }
    if validate_diagnostic_coverage:
        report.update(
            {
                "strict_diagnostic_coverage_ready_count": sum(
                    1 for row in rows if row.get("strict_diagnostic_coverage_ready") is True
                ),
            }
        )
    write_json(summary_path or output_dir / "stages" / "quality" / "summary.json", compact_quality_report(report))
    return report


def compact_quality_report(report: dict[str, Any]) -> dict[str, Any]:
    """生成写入 stage summary 的紧凑 quality payload。"""

    compact = {
        "schema": report.get("schema"),
        "stage": report.get("stage"),
        "quality_options": report.get("quality_options", {}),
        "run_count": report.get("run_count"),
        "config_ids": report.get("config_ids", []),
        "input_ids": report.get("input_ids", []),
        "workflow_input_ready": report.get("workflow_input_ready"),
        "workflow_input_ready_count": report.get("workflow_input_ready_count"),
        "state_model_input_ready_count": report.get("state_model_input_ready_count"),
        "artifact_ready_count": report.get("artifact_ready_count"),
        "sequence_check_display_enabled": report.get("sequence_check_display_enabled"),
        "input_sequence_match_count": report.get("input_sequence_match_count"),
        "input_workload_signatures": compact_input_workload_signatures(report.get("input_workload_signatures")),
        "note": (
            "This summary keeps only stage-level workflow gate fields. Full per-run audit payloads live under artifacts/quality."
        ),
    }
    if "strict_diagnostic_coverage_ready_count" in report:
        compact["strict_diagnostic_coverage_ready_count"] = report.get("strict_diagnostic_coverage_ready_count")
    return compact


def compact_input_workload_signatures(value: Any) -> dict[str, dict[str, Any]]:
    """压缩同 input workload contract 摘要，去掉大体积 signature 列表。"""

    inputs = value if isinstance(value, dict) else {}
    result: dict[str, dict[str, Any]] = {}
    for input_id, item in sorted(inputs.items()):
        if not isinstance(item, dict):
            continue
        row = {
            "input_id": str(input_id),
            "run_count": item.get("run_count"),
            "config_ids": item.get("config_ids", []),
            "signature_count": item.get("signature_count"),
            "signature_match": item.get("signature_match"),
            "sequence_signature_count": item.get("sequence_signature_count"),
            "sequence_match": item.get("sequence_match"),
            "canonical_workload_ready": item.get("canonical_workload_ready"),
            "input_contract_ready": item.get("input_contract_ready"),
        }
        if "sequence_check" in item:
            row["sequence_check"] = item.get("sequence_check")
        for key in (
            "forced_token_enabled_count",
            "forced_token_plan_signature_count",
            "forced_token_plan_signature_match",
            "forced_token_bundle_signature_count",
            "forced_token_bundle_signature_match",
            "forced_token_bundle_ids",
        ):
            if key in item:
                row[key] = item.get(key)
        result[str(input_id)] = row
    return result


def public_quality_row(row: dict[str, Any]) -> dict[str, Any]:
    """去掉 workflow 内部临时字段后写入 report。"""

    return {key: value for key, value in row.items() if not str(key).startswith("_")}


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


def build_workload_signature(
    run: ProfileRun,
    *,
    include_sequence_events: bool = False,
) -> dict[str, Any]:
    """基于 workload identity fact 构造同 input 可比较的 workload 签名。"""

    roles = set(WORKLOAD_SIGNATURE_ROLES)
    events, unknown_roles, unmapped_requests = extract_audit_events(list(run.python_probe_files), run.run_id, roles)
    path_contract = workload_identity_path_contract(list(run.python_probe_files), roles, sample=0)
    by_role: dict[str, collections.Counter[str]] = {role: collections.Counter() for role in roles}
    for event in events:
        by_role.setdefault(event.role, collections.Counter())[event.signature] += 1
    payload = {
        "roles": {
            role: [{"signature": signature, "count": count} for signature, count in sorted(counter.items())]
            for role, counter in sorted(by_role.items())
        }
    }
    encoded = canonical_json(payload)
    signature = "sha256_json:" + hashlib.sha256(encoded.encode("utf-8")).hexdigest()
    sequence_payload = workload_sequence_payload(events)
    sequence_encoded = canonical_json(sequence_payload)
    sequence_signature = "sha256_json:" + hashlib.sha256(sequence_encoded.encode("utf-8")).hexdigest()
    unknown = dict(sorted(unknown_roles.items()))
    unmapped = dict(sorted(unmapped_requests.items()))
    result = {
        "signature": signature,
        "ready": bool(events) and not unknown and not unmapped and bool(path_contract.get("ready")),
        "sequence_signature": sequence_signature,
        "sequence_ready": bool(events) and not unknown and not unmapped,
        "request_event_count": len(events),
        "roles": sorted(roles),
        "role_counts": {role: sum(counter.values()) for role, counter in sorted(by_role.items())},
        "unknown_workload_identity_roles": unknown,
        "unmapped_request_id_events": unmapped,
        "workload_identity_path_contract": path_contract,
    }
    if include_sequence_events:
        result["sequence_events"] = workload_sequence_events(events)
    return result


def workload_sequence_payload(events: list[Any]) -> list[dict[str, str]]:
    """生成严格 sequence 签名使用的最小 payload。"""

    return [
        {
            "role": str(event.role),
            "signature": str(event.signature),
        }
        for event in events
    ]


def workload_sequence_events(events: list[Any]) -> list[dict[str, Any]]:
    """生成可读 sequence mismatch 诊断使用的事件摘要。"""

    return [
        {
            "ordinal": event.ordinal,
            "role": event.role,
            "signature": event.signature,
            "target_id": event.target_id,
            "event_name": event.event_name,
            "seq_no": event.seq_no,
            "request_id": event.request_id,
            "request_fingerprint": event.request_fingerprint,
            "cache_scope": event.cache_scope,
        }
        for event in events
    ]


def summarize_workload_sequence_input_group(
    input_rows: list[dict[str, Any]],
    *,
    include_details: bool,
    sample_limit: int = 8,
) -> dict[str, Any]:
    """汇总同 input 下跨 config 的 workload identity sequence 一致性。"""

    sequence_signatures = sorted(
        {
            str(row.get("canonical_workload_sequence_signature"))
            for row in input_rows
            if row.get("canonical_workload_sequence_signature")
        }
    )
    sequence_match = len(sequence_signatures) == 1
    summary: dict[str, Any] = {
        "sequence_signature_count": len(sequence_signatures),
        "sequence_match": sequence_match,
    }
    if not include_details:
        return summary

    sorted_rows = sorted(input_rows, key=lambda row: str(row.get("config_id") or ""))
    baseline = sorted_rows[0] if sorted_rows else {}
    baseline_events = baseline.get("_workload_sequence_events")
    baseline_sequence = baseline_events if isinstance(baseline_events, list) else []
    mismatches: list[dict[str, Any]] = []
    for row in sorted_rows[1:]:
        current_events = row.get("_workload_sequence_events")
        current_sequence = current_events if isinstance(current_events, list) else []
        mismatch = first_workload_sequence_mismatch(baseline_sequence, current_sequence)
        if mismatch is None:
            continue
        mismatches.append(
            {
                "baseline_config_id": baseline.get("config_id"),
                "config_id": row.get("config_id"),
                **mismatch,
            }
        )

    summary["sequence_check"] = {
        "enabled": True,
        "match": sequence_match,
        "baseline_config_id": baseline.get("config_id"),
        "mismatch_count": len(mismatches),
        "mismatch_samples": mismatches[:sample_limit],
    }
    return summary


def first_workload_sequence_mismatch(
    source: list[dict[str, Any]],
    target: list[dict[str, Any]],
) -> dict[str, Any] | None:
    """定位两个 workload identity sequence 的第一处 `(role, signature)` 错位。"""

    limit = min(len(source), len(target))
    for index in range(limit):
        if sequence_compare_key(source[index]) != sequence_compare_key(target[index]):
            return {
                "index": index,
                "source": source[index],
                "target": target[index],
            }
    if len(source) != len(target):
        return {
            "index": limit,
            "source": source[limit] if limit < len(source) else None,
            "target": target[limit] if limit < len(target) else None,
        }
    return None


def sequence_compare_key(event: dict[str, Any]) -> tuple[str, str]:
    """返回 sequence strict check 的比较键。"""

    return str(event.get("role") or ""), str(event.get("signature") or "")


def workload_timeline_summary(events: list[Any]) -> dict[str, Any]:
    """生成 workload identity timeline 的可读审计摘要。"""

    role_sequence: list[str] = []
    request_sequence: list[str] = []
    batch_groups: list[dict[str, Any]] = []
    for ordinal, event in enumerate(events):
        role_sequence.append(event.role)
        if event.role == "cache_extend_input":
            batch_paths = event.fields.get("batch_paths")
            entries = batch_paths if isinstance(batch_paths, list) else []
            batch_groups.append(
                {
                    "ordinal": ordinal,
                    "batch_size": event.fields.get("batch_size"),
                    "requests": [
                        {
                            "index": entry.get("index"),
                            "request_fingerprint": entry.get("request_fingerprint"),
                            "token_count": entry.get("token_count"),
                        }
                        for entry in entries
                        if isinstance(entry, dict)
                    ],
                }
            )
            request_sequence.append(
                "batch:"
                + ",".join(
                    str(entry.get("request_fingerprint") or "unmapped_request")
                    for entry in entries
                    if isinstance(entry, dict)
                )
            )
            continue
        request_sequence.append(event.request_fingerprint or "unscoped")
    return {
        "role_sequence": role_sequence,
        "request_sequence": request_sequence,
        "cache_extend_batch_groups": batch_groups,
    }


def state_model_input_ready(
    profile_audit: dict[str, Any],
    workload_signature: dict[str, Any],
) -> bool:
    """判断 profile 是否满足 C++ state model 的 workload identity 输入要求。"""

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
    """判断当前 validation workflow 是否可继续。"""

    if not state_ready:
        return False
    if not require_validation_evidence:
        return True
    if profile_audit.get("validator_evidence_ready") is not True:
        return False
    return True
