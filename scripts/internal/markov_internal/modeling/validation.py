"""modeling run 的 validation artifact 组装工具。"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from ..common.io import load_json, write_json
from ..common.paths import resolve_repo_path
from ..hicache.oracle_coverage import (
    build_request_transition_coverage,
    build_transition_coverage,
)
from ..hicache.oracle_delta import (
    build_event_delta_validation,
    build_timeline_delta_validation,
)
from ..hicache.oracle_mismatch import (
    first_hicache_mismatch,
)
from ..hicache.oracle_records import (
    load_predicted_state_records,
)
from ..hicache.oracle_capacity import (
    build_hicache_capacity_config_audit,
    extract_hicache_capacity_oracle_state,
    observed_max_derived_state_counts,
)
from ..hicache.oracle_state import (
    diff_hicache_sets,
    event_base_name,
    extract_hicache_state_snapshots,
    final_state_counts,
    latest_derived_state,
    normalize_hicache_state_for_oracle_compare,
    unchecked_model_state_keys,
)
from .workload import WorkloadWindow, optional_float


def hicache_state_validation_enabled(config: dict[str, Any]) -> bool:
    """判断当前 modeling config 是否启用 HiCache state validation。"""

    validation_cfg = config.get("validation") if isinstance(config.get("validation"), dict) else {}
    hicache_cfg = validation_cfg.get("hicache_state") if isinstance(validation_cfg.get("hicache_state"), dict) else {}
    return bool(hicache_cfg.get("enabled", False))


def write_hicache_predicted_state_trace_if_available(module_summary_path: Path, output_dir: Path) -> Path | None:
    """把 C++ HiCache summary 中的 transition trace 拆成验证专用输出。"""

    if not module_summary_path.is_file():
        return None
    try:
        module_summary = load_json(module_summary_path)
    except json.JSONDecodeError:
        return None
    hicache_summary = extract_hicache_summary(module_summary)
    if not hicache_summary:
        return None

    rows = []
    for row in hicache_summary.get("transition_trace", []):
        if not isinstance(row, dict):
            continue
        pages = [str(item) for item in row.get("pages", []) if item is not None]
        rows.append(
            {
                "request_id": row.get("request_id") or "",
                "operation_id": row.get("operation_id") or "",
                "source_fact_id": f"trace_event:{row.get('source_event_index', '')}",
                "source_event_index": row.get("source_event_index"),
                "source_event_name": row.get("event_name") or "",
                "cache_scope": row.get("cache_scope") or "",
                "ts": row.get("ts"),
                "event_base_name": event_base_name(str(row.get("event_name") or "")),
                "target_page_set": pages,
                "decision_kind": "state_prediction",
                "decision_reason": "derived_from_hicache_fact",
                "transition_kind": row.get("kind") or "",
                "tier_src": tier_src_from_transition(row),
                "tier_dst": tier_dst_from_transition(row),
                "before_state_digest": row.get("before_state_digest") or "",
                "after_state_digest": row.get("after_state_digest") or "",
                "predicted_operation_kind": predicted_operation_kind_from_transition(row),
                "blocking_class": "unknown",
                "unresolved_inputs": [],
            }
        )

    output_path = output_dir / "predicted_target_cache_state_trace.json"
    write_json(
        output_path,
        {
            "schema": "trace_sim.hicache.predicted_state_trace.v1",
            "source": "cpp_hicache_module",
            "record_count": len(rows),
            "records": rows,
            "final_state": hicache_summary.get("final_state", {}),
            "missing_state_model_facts": hicache_summary.get("missing_state_model_facts", {}),
            "skipped_non_state_model_events": hicache_summary.get("skipped_non_state_model_events", 0),
            "target_config": hicache_summary.get("target_config", {}),
            "dag_mutations": hicache_summary.get("dag_mutations", 0),
        },
    )
    return output_path


def tier_src_from_transition(row: dict[str, Any]) -> str:
    """把 C++ transition 行映射为 validation 使用的源 tier。"""

    kind = str(row.get("kind") or "")
    tier = str(row.get("tier") or "")
    if kind.startswith("remove_"):
        return tier
    return ""


def tier_dst_from_transition(row: dict[str, Any]) -> str:
    """把 C++ transition 行映射为 validation 使用的目标 tier。"""

    kind = str(row.get("kind") or "")
    tier = str(row.get("tier") or "")
    if kind.startswith("add_"):
        return tier
    return ""


def predicted_operation_kind_from_transition(row: dict[str, Any]) -> str:
    """把 C++ transition kind 归类为 oracle delta 对比使用的 operation kind。"""

    kind = str(row.get("kind") or "")
    if kind.startswith("add_") or kind.startswith("remove_"):
        return "resident_state_update"
    if kind.startswith("mark_") or kind.startswith("clear_"):
        return "page_metadata_update"
    return kind or "unknown"


def write_hicache_recommended_cpp_config_if_available(validation: dict[str, Any], output_dir: Path) -> Path | None:
    """把 HiCache 推荐配置写成可直接传给 C++ TraceGraph 的 model config。"""

    hicache_state = validation.get("hicache_state")
    if not isinstance(hicache_state, dict):
        return None
    capacity_audit = hicache_state.get("capacity_config_audit")
    if not isinstance(capacity_audit, dict):
        return None
    recommended = capacity_audit.get("recommended_target_config")
    if not isinstance(recommended, dict) or not recommended.get("ready"):
        return None
    hicache = recommended.get("hicache")
    if not isinstance(hicache, dict):
        return None
    payload = {
        "modules": ["hicache"],
        "hicache": {str(key): value for key, value in hicache.items() if value is not None},
    }
    output_path = output_dir / "recommended_hicache_cpp_model_config.json"
    write_json(output_path, payload)
    return output_path


def build_validation(
    mode: str,
    prediction: dict[str, Any],
    run_summary: dict[str, Any],
    workload_window: WorkloadWindow | None,
    trace_paths: list[Path],
    config: dict[str, Any],
    module_summary_path: Path,
    predicted_state_trace_path: Path | None,
    hicache_oracle_trace_paths: list[Path] | None = None,
) -> dict[str, Any]:
    """构造 modeling validation 输出。"""

    validation_cfg = config.get("validation") if isinstance(config.get("validation"), dict) else {}
    threshold = float(validation_cfg.get("faithful_replay_full_e2e_rel_error_max", 0.05))
    summary_real = optional_float(run_summary.get("real_e2e_ns"))
    actual = int(summary_real) if summary_real and summary_real > 0 else None
    predicted = int(prediction["predicted_e2e_ns"])
    rel_error = abs(predicted - actual) / actual if actual else None
    errors: list[str] = []
    if mode == "faithful_replay" and actual is None:
        errors.append("missing_trace_real_e2e")
    if mode == "faithful_replay" and actual and rel_error is not None and rel_error > threshold:
        errors.append("faithful_replay_full_e2e_error_too_high")
    result = {
        "mode": mode,
        "engine": "cpp_trace_graph",
        "validation_ready": not errors,
        "validation_errors": errors,
        "thresholds": {"faithful_replay_full_e2e_rel_error_max": threshold},
        "trace_files": [str(path) for path in trace_paths],
        "dag": {
            "node_count": run_summary.get("node_count"),
            "edge_count": run_summary.get("edge_count"),
            "parsed_record_count": run_summary.get("parsed_record_count"),
            "edge_counts_by_kind": run_summary.get("edge_counts_by_kind"),
            "stage_timings_ms": run_summary.get("stage_timings_ms"),
            "dag_mutation_count": 0,
        },
        "workload_window": {
            "used": workload_window is not None,
            "report_path": str(workload_window.report_path) if workload_window else None,
            "source": workload_window.source if workload_window else None,
            "actual_e2e_ns": workload_window.actual_e2e_ns if workload_window else None,
        },
        "e2e": {
            "predicted_e2e_ns": predicted,
            "actual_e2e_ns": actual,
            "actual_source": "trace_real_e2e_ns" if actual is not None else None,
            "absolute_error_ns": predicted - actual if actual else None,
            "relative_error": rel_error,
        },
    }
    hicache_validation = build_hicache_state_validation_if_enabled(
        validation_cfg,
        trace_paths,
        module_summary_path,
        predicted_state_trace_path,
        hicache_oracle_trace_paths or [],
    )
    if hicache_validation is not None:
        result["hicache_state"] = hicache_validation
        if hicache_validation.get("oracle_state_validation_required", False):
            if not hicache_validation.get("state_trace_ready", False):
                errors.append("hicache_state_trace_not_ready")
            if hicache_validation.get("state_trace_ready") and hicache_validation.get("final_state_match") is False:
                errors.append("hicache_final_state_mismatch")
        if not hicache_validation.get("state_model_fact_ready", False):
            errors.append("hicache_state_model_fact_not_ready")
        result["validation_errors"] = errors
        result["validation_ready"] = not errors
    return result


def build_hicache_state_validation_if_enabled(
    validation_cfg: dict[str, Any],
    trace_paths: list[Path],
    module_summary_path: Path,
    predicted_state_trace_path: Path | None,
    oracle_trace_paths_override: list[Path] | None = None,
) -> dict[str, Any] | None:
    """构造 HiCache state validation。"""

    hicache_cfg = validation_cfg.get("hicache_state") if isinstance(validation_cfg.get("hicache_state"), dict) else {}
    if not bool(hicache_cfg.get("enabled", False)):
        return None

    oracle_paths = list(oracle_trace_paths_override or [])
    if not oracle_paths:
        oracle_paths = [required_repo_path(path) for path in hicache_cfg.get("oracle_trace_paths", []) if isinstance(path, str)]
    if not oracle_paths:
        oracle_paths = trace_paths
    oracle_required = bool(hicache_cfg.get("require_oracle_state_trace", False))
    oracle_page_key_mode = str(hicache_cfg.get("oracle_page_key_mode") or "strip_scope")

    model_summary = load_json(module_summary_path) if module_summary_path.is_file() else {}
    hicache_summary = extract_hicache_summary(model_summary)
    snapshots = extract_hicache_state_snapshots(oracle_paths)
    oracle_final = latest_derived_state(snapshots)
    capacity_oracle = extract_hicache_capacity_oracle_state(snapshots)
    oracle_observed_max_counts = observed_max_derived_state_counts(snapshots)
    model_final = hicache_summary.get("final_state") if isinstance(hicache_summary.get("final_state"), dict) else {}
    ignored_state_keys = configured_ignore_state_keys(hicache_cfg)
    raw_sets_diff = diff_hicache_sets(model_final, oracle_final)
    raw_active_sets_diff = {key: value for key, value in raw_sets_diff.items() if key not in ignored_state_keys}
    raw_ignored_sets_diff = {key: value for key, value in raw_sets_diff.items() if key in ignored_state_keys}
    normalized_model_final = normalize_hicache_state_for_oracle_compare(model_final, oracle_page_key_mode)
    normalized_oracle_final = normalize_hicache_state_for_oracle_compare(oracle_final, oracle_page_key_mode)
    all_sets_diff = diff_hicache_sets(normalized_model_final, normalized_oracle_final)
    sets_diff = {key: value for key, value in all_sets_diff.items() if key not in ignored_state_keys}
    ignored_sets_diff = {key: value for key, value in all_sets_diff.items() if key in ignored_state_keys}
    capacity_config_audit = build_hicache_capacity_config_audit(
        capacity_oracle,
        hicache_summary.get("target_config") if isinstance(hicache_summary.get("target_config"), dict) else {},
        final_state_counts(oracle_final),
        oracle_observed_max_counts,
    )
    predicted_records = load_predicted_state_records(predicted_state_trace_path)
    first_mismatch = first_hicache_mismatch(sets_diff, predicted_records)
    raw_first_mismatch = first_hicache_mismatch(raw_active_sets_diff, predicted_records)
    request_transition_coverage = build_request_transition_coverage(predicted_records, snapshots)
    transition_coverage = build_transition_coverage(predicted_records, snapshots)
    event_delta_validation = build_event_delta_validation(predicted_records, snapshots)
    timeline_delta_validation = build_timeline_delta_validation(predicted_records, snapshots)
    skipped_non_state_model = int(hicache_summary.get("skipped_non_state_model_events", 0) or 0) if hicache_summary else 0
    missing_state_model_facts = []
    missing_state_model_counts = hicache_summary.get("missing_state_model_facts", {}) if hicache_summary else {}
    if isinstance(missing_state_model_counts, dict):
        missing_state_model_facts.extend(sorted(str(key) for key, value in missing_state_model_counts.items() if int(value or 0) > 0))

    return {
        "state_trace_ready": bool(snapshots),
        "state_trace_events": len(snapshots),
        "oracle_state_validation_required": oracle_required,
        "oracle_page_key_mode": oracle_page_key_mode,
        "model_transition_events": len(hicache_summary.get("transition_trace", []) if hicache_summary else []),
        "final_state_match": None if not oracle_final else not first_mismatch,
        "raw_final_state_match": None if not oracle_final else not raw_first_mismatch,
        "sets_diff_by_tier": sets_diff,
        "raw_sets_diff_by_tier": raw_active_sets_diff,
        "ignored_state_keys": sorted(ignored_state_keys),
        "ignored_sets_diff_by_tier": ignored_sets_diff,
        "raw_ignored_sets_diff_by_tier": raw_ignored_sets_diff,
        "model_final_state_counts": final_state_counts(model_final),
        "oracle_final_state_counts": final_state_counts(oracle_final),
        "normalized_model_final_state_counts": final_state_counts(normalized_model_final),
        "normalized_oracle_final_state_counts": final_state_counts(normalized_oracle_final),
        "oracle_observed_max_state_counts": oracle_observed_max_counts,
        "unchecked_model_state_keys": unchecked_model_state_keys(normalized_model_final, normalized_oracle_final),
        "first_mismatch": first_mismatch,
        "raw_first_mismatch": raw_first_mismatch,
        "request_transition_coverage": request_transition_coverage,
        "transition_coverage": transition_coverage,
        "event_delta_validation": event_delta_validation,
        "timeline_delta_validation": timeline_delta_validation,
        "oracle_capacity_summary": capacity_oracle,
        "capacity_config_audit": capacity_config_audit,
        "skipped_non_state_model_events": skipped_non_state_model,
        "unmatched_state_trace_events": 0 if snapshots else None,
        "state_model_fact_ready": bool(hicache_summary) and not missing_state_model_facts,
        "missing_state_model_facts": missing_state_model_facts,
        "missing_state_model_fact_counts": missing_state_model_counts if isinstance(missing_state_model_counts, dict) else {},
        "oracle_trace_files": [str(path) for path in oracle_paths],
        "model_summary_ready": bool(hicache_summary),
        "predicted_state_trace_path": str(predicted_state_trace_path) if predicted_state_trace_path else None,
        "predicted_state_trace_ready": predicted_state_trace_path is not None and predicted_state_trace_path.is_file(),
    }


def configured_ignore_state_keys(hicache_cfg: dict[str, Any]) -> set[str]:
    """读取 validation-only 的 final state diff 忽略字段。"""

    raw = hicache_cfg.get("ignore_state_keys")
    if not isinstance(raw, list):
        return set()
    return {str(item) for item in raw if isinstance(item, str) and item}


def extract_hicache_summary(model_summary: dict[str, Any]) -> dict[str, Any]:
    """从 C++ module summary 中提取 HiCache summary。"""

    modules = model_summary.get("modules")
    if not isinstance(modules, list):
        return {}
    for module in modules:
        if isinstance(module, dict) and isinstance(module.get("hicache"), dict):
            return module["hicache"]
    return {}


def required_repo_path(value: Any) -> Path:
    """解析必填 repo path，缺失时抛出错误。"""

    path = resolve_repo_path(value)
    if path is None:
        raise ValueError("expected a non-empty path")
    return path
