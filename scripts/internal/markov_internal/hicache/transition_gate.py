#!/usr/bin/env python3
"""HiCache transition operation gate 产物生成。

本模块把模型 transition 和 target-side observed operation 聚合成诊断 gate。
这些 gate 只服务于 exactness 分类与后续 DAG patch 过滤设计，不产生 patch action。
"""

from __future__ import annotations

import collections
from pathlib import Path
from typing import Any

from ..common.io import load_json, write_json
from ..common.paths import ROOT_DIR
from .oracle_state import normalize_hicache_page_key

from .transition_taxonomy import (
    MARKER_DELTA_KINDS,
    NOISE_OBSERVED_OPERATION_KINDS,
    PHYSICAL_CANDIDATE_OPERATION_KINDS,
    STATE_ONLY_OPERATION_KINDS,
    append_unique,
    list_dicts,
    load_hicache_summary,
)

TRANSITION_PAGE_FIELDS = (
    "pages",
    "target_page_set",
    "host_pages",
    "lock_pages",
    "prefix_pages",
    "suffix_pages",
    "hit_pages",
)

OBSERVED_ROLE_TO_OPERATION_KIND = {
    "all_blocks_cleared_observed": "host_cleanup",
    "capacity_request": "capacity_request",
    "capacity_result_observed": "capacity_result",
    "host_mem_release_enqueue_observed": "host_cleanup",
    "node_remove_observed": "host_cleanup",
    "insert_result_observed": "request_insert",
    "node_store_observed": "storage_backup",
    "write_enqueue_observed": "write_through_backup",
    "write_start_observed": "write_through_backup",
    "write_ack_checkpoint_observed": "write_through_backup",
    "write_counter_delta_observed": "write_through_backup",
    "writeback_enqueue_observed": "write_back_flush",
    "writeback_io_observed": "write_back_flush",
    "writeback_schedule_observed": "write_back_flush",
    "writeback_storage_schedule_observed": "write_back_flush",
    "prefetch_check_point": "prefetch_checkpoint",
    "prefetch_decision_observed": "prefetch_plan",
    "prefetch_enqueue_observed": "prefetch_plan",
    "prefetch_intent_observed": "prefetch_plan",
    "prefetch_loaded_tokens_observed": "prefetch_ready",
    "prefetch_progress_observed": "prefetch_ready",
    "prefetch_rate_limit_observed": "prefetch_revoke",
    "storage_hit_query_observed": "prefetch_ready",
    "lock_scope_delta": "lock_ref",
    "lock_scope_result_observed": "lock_ref",
    "host_ref_delta_observed": "lock_ref",
    "load_start_observed": "device_loadback",
    "request_admission_observed": "request_admission",
    "lookup_result_observed": "request_lookup",
    "request_lifecycle_path_observed": "request_lifecycle",
    "request_lifecycle_runtime_observed": "request_lifecycle",
    "storage_control_drain_boundary": "storage_control",
}



def resolve_repo_path(path: Path) -> Path:
    """把 repo 相对路径解析为绝对路径。"""

    path = path.expanduser()
    if path.is_absolute():
        return path
    return ROOT_DIR / path


def load_predicted_trace(path: Path) -> dict[str, Any]:
    """读取 predicted state trace，并做基本结构检查。"""

    if not path.is_file():
        raise FileNotFoundError(f"missing predicted trace: {path}")
    payload = load_json(path)
    if not isinstance(payload, dict):
        raise ValueError(f"predicted trace is not a JSON object: {path}")
    if not isinstance(payload.get("records"), list):
        payload["records"] = []
    return payload


def predicted_records(predicted: dict[str, Any]) -> list[dict[str, Any]]:
    """读取 predicted trace records。"""

    records = predicted.get("records")
    if not isinstance(records, list):
        return []
    return [record for record in records if isinstance(record, dict)]


def record_pages(record: dict[str, Any]) -> list[str]:
    """读取 transition record 的 page 集合。"""

    pages = record.get("target_page_set")
    if not isinstance(pages, list):
        return []
    return [str(page) for page in pages if page is not None]


def canonical_request_key_from_row(row: dict[str, Any]) -> str:
    """生成保守的 run-local canonical request key。"""

    request_id = str(row.get("request_id") or "")
    cache_scope = str(row.get("cache_scope") or "")
    operation_id = str(row.get("operation_id") or "")
    if request_id:
        return f"{cache_scope}:{request_id}"
    if operation_id:
        return f"{cache_scope}:operation:{operation_id}"
    return cache_scope


def build_transition_patch_gate_scoreboard_from_entries(
    matrix_dir: Path,
    prediction_entries: list[dict[str, Any]],
    *,
    page_key_mode: str,
    sample_limit: int,
) -> dict[str, Any]:
    """从 compare 分类 entries 构建 patch gate scoreboard。"""

    rows: list[dict[str, Any]] = []
    skipped: list[dict[str, Any]] = []
    for prediction_entry in prediction_entries:
        paths = prediction_gate_input_paths(prediction_entry)
        if paths.get("skip_reason"):
            skipped.append(serializable_gate_skip(paths))
            continue
        rows.append(
            write_prediction_gate_outputs(
                paths["prediction_dir"],
                paths["observed_path"],
                prediction_entry,
                page_key_mode=page_key_mode,
                sample_limit=sample_limit,
            )
        )
    return {
        "schema": "trace_sim.hicache.transition_patch_gate_scoreboard.v1",
        "matrix_dir": str(matrix_dir),
        "prediction_count": len(rows),
        "skipped_prediction_count": len(skipped),
        "operation_gate_schema_ready_count": sum(1 for row in rows if row.get("operation_gate_schema_ready")),
        "transition_coverage_ready_count": sum(1 for row in rows if row.get("transition_coverage_ready")),
        "state_marker_filter_ready_count": sum(1 for row in rows if row.get("state_marker_filter_ready")),
        "unresolved_report_ready_count": sum(1 for row in rows if row.get("unresolved_report_ready")),
        "ready": bool(rows)
        and not skipped
        and all(row.get("operation_gate_schema_ready") for row in rows)
        and all(row.get("transition_coverage_ready") for row in rows)
        and all(row.get("state_marker_filter_ready") for row in rows)
        and all(row.get("unresolved_report_ready") for row in rows),
        "patch_allowed": False,
        "by_family": summarize_gate_rows_by_key(rows, "transition_family"),
        "by_target_config": summarize_gate_rows_by_key(rows, "target_config_id"),
        "predictions": rows,
        "skipped_predictions": skipped[:sample_limit],
        "notes": [
            "This artifact contains diagnostic operation gates only; it does not emit patch actions.",
            "Readiness fields only validate gate coverage and filtering boundaries, not source attribution or DAG patch readiness.",
        ],
    }


def prediction_gate_input_paths(prediction_entry: dict[str, Any]) -> dict[str, Any]:
    """解析 gate 生成所需路径，缺失时返回可报告的 skip reason。"""

    prediction_dir_raw = str(prediction_entry.get("prediction_dir") or "")
    observed_path_raw = str(prediction_entry.get("observed_target_trace_path") or "")
    prediction_dir = resolve_repo_path(Path(prediction_dir_raw)) if prediction_dir_raw else None
    observed_path = resolve_repo_path(Path(observed_path_raw)) if observed_path_raw else None
    base = {
        "label": prediction_entry.get("label"),
        "input_id": prediction_entry.get("input_id"),
        "source_config_id": prediction_entry.get("source_config_id"),
        "target_config_id": prediction_entry.get("target_config_id"),
        "prediction_dir": prediction_dir,
        "observed_path": observed_path,
    }
    if not prediction_dir_raw:
        return {**base, "skip_reason": "missing_prediction_dir"}
    if not observed_path_raw:
        return {**base, "skip_reason": "missing_observed_target_trace_path"}
    predicted_trace = prediction_dir / "predicted_target_cache_state_trace.json"
    if not predicted_trace.is_file():
        return {**base, "skip_reason": "missing_predicted_trace", "predicted_trace": str(predicted_trace)}
    if observed_path is None or not observed_path.is_file():
        return {**base, "skip_reason": "missing_observed_target_trace"}
    return base


def serializable_gate_skip(paths: dict[str, Any]) -> dict[str, Any]:
    """把 gate skip 诊断行转成 JSON 可写结构。"""

    result = dict(paths)
    for key in ("prediction_dir", "observed_path"):
        if isinstance(result.get(key), Path):
            result[key] = str(result[key])
    return result


def write_prediction_gate_outputs(
    prediction_dir: Path,
    observed_path: Path,
    prediction_entry: dict[str, Any],
    *,
    page_key_mode: str,
    sample_limit: int,
) -> dict[str, Any]:
    """为单个 prediction 写出 operation gate 产物并返回 scoreboard row。"""

    model_payload, observed_payload, coverage, row = build_prediction_gate_artifacts(
        prediction_dir,
        observed_path,
        prediction_entry,
        page_key_mode=page_key_mode,
        sample_limit=sample_limit,
    )
    write_json(prediction_dir / "transition_operation_gate_model.json", model_payload)
    write_json(prediction_dir / "transition_operation_gate_observed.json", observed_payload)
    write_json(prediction_dir / "transition_patch_gate_coverage.json", coverage)
    return row


def build_prediction_gate_artifacts(
    prediction_dir: Path,
    observed_path: Path,
    prediction_entry: dict[str, Any],
    *,
    page_key_mode: str,
    sample_limit: int,
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any], dict[str, Any]]:
    """构造单个 prediction 的 model/observed gate payload 和 scoreboard row。"""

    predicted = load_predicted_trace(prediction_dir / "predicted_target_cache_state_trace.json")
    observed = load_json(observed_path) if observed_path.is_file() else {}
    hicache_summary = load_hicache_summary(prediction_dir / "model_summary.json")
    model_gates = build_model_operation_gates(
        predicted_records(predicted),
        hicache_summary,
        prediction_entry,
        page_key_mode=page_key_mode,
        sample_limit=sample_limit,
    )
    observed_gates, observed_filter = build_observed_operation_gates(observed, prediction_entry, page_key_mode=page_key_mode, sample_limit=sample_limit)
    coverage = build_transition_patch_gate_coverage(predicted_records(predicted), model_gates, sample_limit=sample_limit)
    model_payload = {
        "schema": "trace_sim.hicache.transition_operation_gate_model.v1",
        "gate_maturity": "diagnostic",
        "patch_allowed": False,
        "prediction_dir": str(prediction_dir),
        "transition_family": prediction_entry.get("family"),
        "operation_gate_count": len(model_gates),
        "operation_gate_count_by_kind": count_gates_by_kind(model_gates),
        "operation_gate_count_by_classification": count_gates_by_field(model_gates, "classification"),
        "operation_gates": model_gates,
        "notes": [
            "Operation gates are used only for transition mismatch classification, coverage, and downstream DAG patch filtering.",
            "patch_allowed is fixed to false in this diagnostic stage.",
        ],
    }
    observed_payload = {
        "schema": "trace_sim.hicache.transition_operation_gate_observed.v1",
        "gate_maturity": "diagnostic",
        "patch_allowed": False,
        "prediction_dir": str(prediction_dir),
        "observed_target_trace_path": str(observed_path),
        "transition_family": prediction_entry.get("family"),
        "oracle_ready": bool(observed.get("oracle_ready")),
        "operation_gate_count": len(observed_gates),
        "operation_gate_count_by_kind": count_gates_by_kind(observed_gates),
        "filter_summary": observed_filter,
        "operation_gates": observed_gates,
        "notes": [
            "Observed operation gates are validation-only evidence.",
            "source_actual and timing_observation are not fed back into the normal state model.",
        ],
    }
    row = {
        "label": prediction_entry.get("label"),
        "input_id": prediction_entry.get("input_id"),
        "source_config_id": prediction_entry.get("source_config_id"),
        "target_config_id": prediction_entry.get("target_config_id"),
        "prediction_dir": str(prediction_dir),
        "transition_family": prediction_entry.get("family"),
        "classification": prediction_entry.get("classification"),
        "patch_risk": prediction_entry.get("patch_risk"),
        "patch_filter_action": prediction_entry.get("patch_filter_action"),
        "source_attribution_required": prediction_entry.get("source_attribution_required"),
        "duration_required": prediction_entry.get("duration_required"),
        "evidence_required": prediction_entry.get("evidence_required"),
        "operation_gate_schema_ready": operation_gate_schema_ready(model_gates),
        "transition_coverage_ready": bool(coverage.get("coverage_ready")),
        "state_marker_filter_ready": bool(coverage.get("state_marker_filter_ready")),
        "unresolved_report_ready": bool(coverage.get("unresolved_report_ready")),
        "transition_count": coverage.get("transition_count"),
        "covered_transition_count": coverage.get("covered_transition_count"),
        "unresolved_transition_count": coverage.get("unresolved_transition_count"),
        "model_operation_gate_count": len(model_gates),
        "observed_operation_gate_count": len(observed_gates),
        "model_operation_gate_count_by_kind": count_gates_by_kind(model_gates),
        "observed_operation_gate_count_by_kind": count_gates_by_kind(observed_gates),
    }
    return model_payload, observed_payload, coverage, row


def build_model_operation_gates(
    records: list[dict[str, Any]],
    hicache_summary: dict[str, Any],
    prediction_entry: dict[str, Any],
    *,
    page_key_mode: str,
    sample_limit: int,
) -> list[dict[str, Any]]:
    """从模型 transition trace 聚合 patch gate 诊断行。"""

    del sample_limit
    operation_provenance = collect_operation_provenance(hicache_summary)
    grouped: dict[str, dict[str, Any]] = {}
    for ordinal, record in enumerate(records):
        transition_kind = str(record.get("transition_kind") or "")
        operation_kind = operation_gate_kind_from_transition(transition_kind)
        classification = operation_gate_classification(operation_kind)
        pages = sorted({normalize_hicache_page_key(page, page_key_mode) for page in record_pages(record)})
        operation_id = str(record.get("operation_id") or "")
        request_key = canonical_request_key_from_row(record)
        grouping_key = operation_gate_grouping_key(record, operation_kind, ordinal)
        item = grouped.setdefault(
            grouping_key,
            {
                "gate_id": f"diagnostic:model:{len(grouped) + 1}",
                "operation_kind": operation_kind,
                "gate_maturity": "diagnostic",
                "patch_allowed": False,
                "operation_class": operation_gate_class(operation_kind),
                "cache_scope": record.get("cache_scope") or "",
                "request_key": request_key,
                "operation_id": operation_id,
                "pages": [],
                "page_count": 0,
                "transition_family": prediction_entry.get("family"),
                "classification": classification,
                "patch_risk": prediction_entry.get("patch_risk"),
                "grouping_confidence": "strong" if operation_id else "weak",
                "provenance": {
                    "transition_ids": [],
                    "transition_ordinals": [],
                    "transition_kinds": [],
                    "policy_decision_epochs": [],
                    "async_lifecycle_epochs": [],
                    "capacity_victim_epochs": [],
                    "ref_mutation_epochs": [],
                    "observed_event_ids": [],
                },
            },
        )
        item["pages"] = sorted(set(item["pages"]) | set(pages))
        item["page_count"] = len(item["pages"])
        transition_id = record.get("transition_id") or ordinal
        item["provenance"]["transition_ids"].append(str(transition_id))
        item["provenance"]["transition_ordinals"].append(ordinal)
        append_unique(item["provenance"]["transition_kinds"], transition_kind)
        if operation_id:
            merge_operation_provenance(item["provenance"], operation_provenance.get(operation_id, {}))
    return sorted(grouped.values(), key=lambda row: str(row.get("gate_id") or ""))


def collect_operation_provenance(hicache_summary: dict[str, Any]) -> dict[str, dict[str, list[Any]]]:
    """按 operation_id 汇总 C++ 账本 provenance。"""

    provenance: dict[str, dict[str, list[Any]]] = {}
    ledger_specs = (
        ("policy_decision_trace", "policy_decision_epochs", "decision_epoch"),
        ("async_lifecycle_trace", "async_lifecycle_epochs", "transition_epoch"),
        ("capacity_victim_choices", "capacity_victim_epochs", "selection_epoch"),
        ("ref_mutation_trace", "ref_mutation_epochs", "mutation_epoch"),
    )
    for ledger_name, output_key, epoch_key in ledger_specs:
        for row in list_dicts(hicache_summary.get(ledger_name, [])):
            operation_id = str(row.get("operation_id") or "")
            if not operation_id:
                continue
            item = provenance.setdefault(operation_id, {})
            append_unique(item.setdefault(output_key, []), row.get(epoch_key))
    return provenance


def merge_operation_provenance(target: dict[str, list[Any]], source: dict[str, list[Any]]) -> None:
    """把 operation-level provenance 合并到 gate provenance。"""

    for key, values in source.items():
        if not isinstance(values, list):
            continue
        out = target.setdefault(key, [])
        for value in values:
            append_unique(out, value)


def operation_gate_grouping_key(record: dict[str, Any], operation_kind: str, ordinal: int) -> str:
    """生成 operation gate grouping key。"""

    operation_id = str(record.get("operation_id") or "")
    cache_scope = str(record.get("cache_scope") or "")
    if operation_id:
        return f"{cache_scope}:op:{operation_id}:{operation_kind}"
    request_key = canonical_request_key_from_row(record)
    source_event_index = str(record.get("source_event_index") or "")
    event_name = str(record.get("source_event_name") or record.get("event_base_name") or "")
    if request_key or source_event_index or event_name:
        return f"{cache_scope}:weak:{request_key}:{source_event_index}:{event_name}:{operation_kind}"
    return f"{cache_scope}:weak:ordinal:{ordinal}:{operation_kind}"


def operation_gate_kind_from_transition(transition_kind: str) -> str:
    """把模型 transition kind 映射到 patch gate operation taxonomy。"""

    if transition_kind in {"mark_dirty", "clear_dirty"}:
        return "dirty_marker"
    if transition_kind in {"mark_evicted", "clear_evicted"}:
        return "evicted_marker"
    if transition_kind in {"mark_backuped", "clear_backuped"}:
        return "backuped_marker"
    if transition_kind in {"acquire_request_ref", "release_request_ref"}:
        return "ref_protection"
    if transition_kind == "increment_hit_count":
        return "hit_count_update"
    if transition_kind in {"add_l1_residency", "restore_l1_residency"}:
        return "request_insert"
    if transition_kind in {"promote_visible_prefix_to_l1", "enqueue_loadback", "complete_loadback"}:
        return "device_loadback"
    if transition_kind == "evict_l1_node":
        return "device_eviction"
    if transition_kind == "evict_host_node":
        return "host_cleanup"
    if transition_kind in {"enqueue_write_through_backup", "complete_write_through_backup", "commit_host_backup"}:
        return "host_backup"
    if transition_kind in {"enqueue_storage_backup", "commit_host_storage_backup", "complete_storage_backup"}:
        return "storage_backup"
    if transition_kind in {"enqueue_writeback", "complete_writeback", "cancel_writeback"}:
        return "write_back_flush"
    if transition_kind == "prefetch_planned":
        return "prefetch_plan"
    if transition_kind == "prefetch_ready":
        return "prefetch_read"
    if transition_kind == "apply_prefetch_host_visibility":
        return "prefetch_apply"
    if transition_kind == "prefetch_terminated":
        return "prefetch_control"
    if transition_kind in {"prefetch_revoked", "prefetch_suppressed", "prefetch_timeout_incomplete"}:
        return "prefetch_revoke"
    return "unresolved"


def operation_gate_class(operation_kind: str) -> str:
    """返回 operation gate 的粗粒度 class。"""

    if operation_kind in {"write_back_flush", "storage_backup", "prefetch_read"}:
        return "physical_io"
    if operation_kind in {"host_backup", "device_loadback"}:
        return "physical_memory"
    if operation_kind in {"host_cleanup", "device_eviction", "prefetch_apply", "request_insert"}:
        return "metadata_control"
    if operation_kind in STATE_ONLY_OPERATION_KINDS:
        return "state_only"
    return "unknown"


def operation_gate_classification(operation_kind: str) -> str:
    """返回 patch gate coverage 使用的 classification。"""

    if operation_kind in STATE_ONLY_OPERATION_KINDS:
        return "state_marker_only"
    if operation_kind in PHYSICAL_CANDIDATE_OPERATION_KINDS:
        return "physical_candidate"
    if operation_kind == "request_insert":
        return "metadata_candidate"
    return "unresolved"


def build_observed_operation_gates(
    observed: dict[str, Any],
    prediction_entry: dict[str, Any],
    *,
    page_key_mode: str,
    sample_limit: int,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """把 target-side observed operations 聚合成 validation-only operation gate。"""

    operations = list_dicts(observed.get("observed_operations", []))
    grouped: dict[str, dict[str, Any]] = {}
    filtered_counts: collections.Counter[str] = collections.Counter()
    for ordinal, row in enumerate(operations):
        operation_kind = operation_gate_kind_from_observed(row)
        if operation_kind in NOISE_OBSERVED_OPERATION_KINDS:
            filtered_counts[operation_kind] += 1
            continue
        pages = row.get("pages") if isinstance(row.get("pages"), list) else []
        normalized_pages = sorted({normalize_hicache_page_key(page, page_key_mode) for page in pages if page is not None})
        operation_id = str(row.get("operation_id") or "")
        request_key = str(row.get("canonical_request_key") or "")
        cache_scope = str(row.get("cache_scope") or "")
        grouping_key = f"{cache_scope}:{request_key}:{operation_id}:{operation_kind}:{row.get('fact_role') or ''}"
        item = grouped.setdefault(
            grouping_key,
            {
                "gate_id": f"diagnostic:observed:{len(grouped) + 1}",
                "operation_kind": operation_kind,
                "gate_maturity": "diagnostic",
                "patch_allowed": False,
                "operation_class": operation_gate_class(operation_kind),
                "cache_scope": cache_scope,
                "request_key": request_key,
                "operation_id": operation_id,
                "pages": [],
                "page_count": 0,
                "transition_family": prediction_entry.get("family"),
                "classification": operation_gate_classification(operation_kind),
                "patch_risk": prediction_entry.get("patch_risk"),
                "evidence_class": observed_evidence_class(row),
                "provenance": {
                    "observed_event_ids": [],
                    "fact_roles": [],
                    "event_kinds": [],
                    "transition_ids": [],
                    "policy_decision_epochs": [],
                    "async_lifecycle_epochs": [],
                    "capacity_victim_epochs": [],
                    "ref_mutation_epochs": [],
                },
            },
        )
        item["pages"] = sorted(set(item["pages"]) | set(normalized_pages))
        item["page_count"] = len(item["pages"])
        append_unique(item["provenance"]["observed_event_ids"], row.get("observed_operation_id") or ordinal, limit=sample_limit)
        append_unique(item["provenance"]["fact_roles"], row.get("fact_role"))
        append_unique(item["provenance"]["event_kinds"], row.get("event_kind"))
    return sorted(grouped.values(), key=lambda item: str(item.get("gate_id") or "")), {
        "input_operation_count": len(operations),
        "filtered_noise_counts": dict(sorted(filtered_counts.items())),
        "emitted_group_count": len(grouped),
    }


def operation_gate_kind_from_observed(row: dict[str, Any]) -> str:
    """把 observed operation 规整为 patch gate operation kind。"""

    operation_kind = str(row.get("operation_kind") or "")
    fact_role = str(row.get("fact_role") or "")
    mapped = OBSERVED_ROLE_TO_OPERATION_KIND.get(fact_role) or operation_kind or "unknown"
    if mapped in {"capacity_request", "capacity_result"}:
        return "allocator_pressure"
    if mapped in {"lock_ref"}:
        return "ref_protection"
    if mapped in {"write_through_backup"}:
        return "host_backup"
    if mapped == "prefetch_ready":
        return "prefetch_read"
    if mapped == "prefetch_checkpoint":
        return "prefetch_apply"
    if mapped in {"maintenance_checkpoint", "request_lookup", "request_lifecycle", "storage_control"}:
        return mapped
    return mapped


def observed_evidence_class(row: dict[str, Any]) -> str:
    """标记 observed operation evidence 的证据等级。"""

    fact_class = str(row.get("fact_class") or "")
    fact_role = str(row.get("fact_role") or "")
    if fact_class == "runtime_model_checkpoint":
        return "control_boundary"
    if fact_class == "source_actual" and fact_role.endswith("_observed"):
        return "exact_physical"
    if fact_role in {"capacity_request", "capacity_result_observed"}:
        return "diagnostic_anchor"
    if "snapshot" in fact_role:
        return "snapshot_delta"
    return "physical_boundary"


def build_transition_patch_gate_coverage(records: list[dict[str, Any]], operation_gates: list[dict[str, Any]], *, sample_limit: int) -> dict[str, Any]:
    """检查 operation gate 是否覆盖所有模型 transition。"""

    covered_ordinals: set[int] = set()
    unresolved: list[dict[str, Any]] = []
    physical_with_marker_kind: list[dict[str, Any]] = []
    for gate in operation_gates:
        provenance = gate.get("provenance", {}) if isinstance(gate.get("provenance"), dict) else {}
        for ordinal in provenance.get("transition_ordinals", []):
            try:
                covered_ordinals.add(int(ordinal))
            except (TypeError, ValueError):
                continue
        if gate.get("classification") == "unresolved":
            unresolved.append({"gate_id": gate.get("gate_id"), "operation_kind": gate.get("operation_kind"), "provenance": provenance})
        if gate.get("classification") == "physical_candidate":
            marker_kinds = sorted(set(provenance.get("transition_kinds", [])) & MARKER_DELTA_KINDS)
            if marker_kinds:
                physical_with_marker_kind.append({"gate_id": gate.get("gate_id"), "operation_kind": gate.get("operation_kind"), "marker_transition_kinds": marker_kinds})
    missing = [
        {
            "ordinal": ordinal,
            "transition_kind": record.get("transition_kind"),
            "source_event_index": record.get("source_event_index"),
        }
        for ordinal, record in enumerate(records)
        if ordinal not in covered_ordinals
    ]
    gate_counts_by_classification = count_gates_by_field(operation_gates, "classification")
    return {
        "schema": "trace_sim.hicache.transition_patch_gate_coverage.v1",
        "transition_count": len(records),
        "covered_transition_count": len(covered_ordinals),
        "missing_transition_count": len(missing),
        "unresolved_transition_count": len(unresolved),
        "operation_gate_count": len(operation_gates),
        "operation_gate_count_by_kind": count_gates_by_kind(operation_gates),
        "operation_gate_count_by_classification": gate_counts_by_classification,
        "physical_candidate_operation_gate_count": int(gate_counts_by_classification.get("physical_candidate", 0)),
        "state_only_operation_gate_count": int(gate_counts_by_classification.get("state_marker_only", 0)),
        "coverage_ready": not missing,
        "state_marker_filter_ready": not physical_with_marker_kind,
        "unresolved_report_ready": not unresolved or bool(unresolved[:sample_limit]),
        "missing_transitions": missing[:sample_limit],
        "unresolved_operation_gates": unresolved[:sample_limit],
        "physical_candidate_marker_violations": physical_with_marker_kind[:sample_limit],
        "notes": [
            "State-only markers must not contaminate physical_candidate operation gates.",
            "Unresolved gates are never silently dropped; samples must be reported in this file.",
        ],
    }


def operation_gate_schema_ready(operation_gates: list[dict[str, Any]]) -> bool:
    """检查 operation gate 最小 schema 是否齐备。"""

    required = ("gate_id", "operation_kind", "gate_maturity", "patch_allowed", "operation_class", "classification", "provenance")
    return bool(operation_gates) and all(
        all(key in gate for key in required) and gate.get("gate_maturity") == "diagnostic" and gate.get("patch_allowed") is False
        for gate in operation_gates
    )


def count_gates_by_field(operation_gates: list[dict[str, Any]], field: str) -> dict[str, int]:
    """按 operation gate 字段计数。"""

    return dict(sorted(collections.Counter(str(row.get(field) or "") for row in operation_gates).items()))

def summarize_gate_rows_by_key(rows: list[dict[str, Any]], key: str) -> dict[str, Any]:
    """按矩阵字段汇总 patch gate readiness。"""

    result: dict[str, Any] = {}
    for value in sorted({str(row.get(key) or "") for row in rows}):
        selected = [row for row in rows if str(row.get(key) or "") == value]
        result[value] = {
            "prediction_count": len(selected),
            "operation_gate_schema_ready_count": sum(1 for row in selected if row.get("operation_gate_schema_ready")),
            "transition_coverage_ready_count": sum(1 for row in selected if row.get("transition_coverage_ready")),
            "state_marker_filter_ready_count": sum(1 for row in selected if row.get("state_marker_filter_ready")),
            "unresolved_report_ready_count": sum(1 for row in selected if row.get("unresolved_report_ready")),
            "model_operation_gate_count": sum(int(row.get("model_operation_gate_count") or 0) for row in selected),
            "observed_operation_gate_count": sum(int(row.get("observed_operation_gate_count") or 0) for row in selected),
            "unresolved_transition_count": sum(int(row.get("unresolved_transition_count") or 0) for row in selected),
        }
    return result


def count_gates_by_kind(operation_gates: list[dict[str, Any]]) -> dict[str, int]:
    """按 operation kind 计数。"""

    counts: collections.Counter[str] = collections.Counter(str(row.get("operation_kind") or "") for row in operation_gates)
    return dict(sorted(counts.items()))
