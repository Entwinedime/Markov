#!/usr/bin/env python3
"""HiCache transition exactness 验证入口。

本脚本是只读诊断入口。它读取 C++ state model 输出的 predicted transition
trace，以及 full Python probe 中的 validation-only evidence，完成 self、cross
或 matrix 方向的 transition 比较。每个 compare 结果都会带上 family 分类和
DAG patch gate 字段；matrix 级 catalog 与 gate scoreboard 是显式开关控制的
派生产物。本脚本不生成 synthetic state-model fact，也不把 target actual / oracle
信息回写到建模输入。
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from hicache_state_matrix import profile_run_from_manifest, safe_slug  # noqa: E402
from hicache_transition_catalog import (  # noqa: E402
    build_transition_mismatch_catalog_from_entries,
    write_transition_catalog_outputs,
)
from hicache_transition_gate import (  # noqa: E402
    build_transition_patch_gate_scoreboard_from_entries,
    write_prediction_gate_outputs,
)
from hicache_transition_taxonomy import (  # noqa: E402
    build_transition_classification_entry,
    compare_result_classification_fields,
    load_hicache_summary,
)
from hicache_fact_contract import parse_fact_or_none  # noqa: E402
from model_runner import (  # noqa: E402
    DELTA_KIND_BY_STATE_KEY,
    build_oracle_timeline_deltas,
    derived_hicache_state_from_snapshot,
    extract_hicache_state_snapshots,
    latest_derived_state,
    normalize_hicache_state_for_oracle_compare,
    normalize_hicache_page_key,
    snapshot_is_completed_state,
    snapshot_timeline_sort_key,
    union_hicache_states,
)
from trace_json import load_chrome_trace_events  # noqa: E402


ROOT_DIR = Path(__file__).resolve().parents[2]
DEFAULT_PAGE_KEY_MODE = "strip_scope"
CLI_DESCRIPTION = (
    "Validate HiCache transition exactness from predicted model transitions "
    "and target-side Python-probe oracle traces."
)

ACTIVE_STATE_KEYS = (
    "l1_resident_pages",
    "l2_resident_pages",
    "l3_resident_pages",
    "dirty_pages",
    "backuped_pages",
    "evicted_pages",
    "locked_pages",
    "pending_writeback_pages",
    "prefetch_planned_pages",
    "prefetch_ready_pages",
    "prefetch_late_pages",
    "prefetch_suppressed_pages",
)

SNAPSHOT_VISIBLE_STATE_KEYS = tuple(DELTA_KIND_BY_STATE_KEY)
TRANSITION_COMPARABLE_STATE_KEYS = tuple(key for key in SNAPSHOT_VISIBLE_STATE_KEYS if key != "locked_pages")
SELF_CHECK_HARD_STATE_KEYS = tuple(key for key in ACTIVE_STATE_KEYS if key != "locked_pages")

STATE_DELTA_KINDS = {
    "l1_resident_pages": ("add_l1_resident", "remove_l1_resident"),
    "l2_resident_pages": ("add_l2_resident", "remove_l2_resident"),
    "l3_resident_pages": ("add_l3_resident", "remove_l3_resident"),
    "dirty_pages": ("mark_dirty", "clear_dirty"),
    "backuped_pages": ("mark_backuped", "clear_backuped"),
    "evicted_pages": ("mark_evicted", "clear_evicted"),
    "locked_pages": ("mark_locked", "clear_locked"),
    "pending_writeback_pages": ("mark_pending_writeback", "clear_pending_writeback"),
    "prefetch_planned_pages": ("prefetch_planned", "clear_prefetch_planned"),
    "prefetch_ready_pages": ("prefetch_ready", "clear_prefetch_ready"),
    "prefetch_late_pages": ("prefetch_late", "clear_prefetch_late"),
    "prefetch_suppressed_pages": ("prefetch_suppressed", "clear_prefetch_suppressed"),
}

KNOWN_TRANSITION_KINDS = {
    "acquire_request_ref",
    "add_l1_residency",
    "apply_prefetch_host_visibility",
    "cancel_writeback",
    "commit_host_backup",
    "commit_host_storage_backup",
    "complete_loadback",
    "complete_storage_backup",
    "complete_write_through_backup",
    "complete_writeback",
    "enqueue_loadback",
    "enqueue_storage_backup",
    "enqueue_write_through_backup",
    "enqueue_writeback",
    "evict_host_node",
    "evict_l1_node",
    "increment_hit_count",
    "mark_dirty",
    "prefetch_planned",
    "prefetch_ready",
    "prefetch_revoked",
    "prefetch_suppressed",
    "prefetch_terminated",
    "prefetch_timeout_incomplete",
    "promote_visible_prefix_to_l1",
    "release_request_ref",
    "restore_l1_residency",
}

CORE_RECORD_FIELDS = (
    "source_event_index",
    "source_event_name",
    "cache_scope",
    "target_page_set",
    "transition_kind",
)

ADVISORY_RECORD_FIELDS = (
    "request_id",
    "operation_id",
    "event_base_name",
    "predicted_operation_kind",
)

LOCK_ACQUIRE_KINDS = {
    "acquire_request_ref",
    "enqueue_loadback",
    "enqueue_storage_backup",
    "enqueue_write_through_backup",
    "enqueue_writeback",
}

LOCK_RELEASE_BY_KIND = {
    "release_request_ref": "request_ref",
    "complete_loadback": "loadback",
    "complete_storage_backup": "storage",
    "complete_write_through_backup": "write_through_backup",
    "complete_writeback": "writeback",
    "cancel_writeback": "writeback",
}

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
    "storage_control_checkpoint_observed": "storage_control",
}


@dataclass(frozen=True)
class PathsForPrediction:
    """一个 prediction 输出目录内的标准产物路径。"""

    prediction_dir: Path
    predicted_trace: Path
    validation: Path
    model_self_check: Path


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """解析 CLI 参数。"""

    parser = argparse.ArgumentParser(description=CLI_DESCRIPTION)
    parser.add_argument(
        "--mode",
        required=True,
        choices=(
            "model-self-check",
            "extract-target-oracle",
            "compare-self",
            "compare-cross",
            "compare-matrix",
        ),
    )
    parser.add_argument("--prediction-dir", type=Path, help="Single prediction output directory.")
    parser.add_argument("--predicted-trace", type=Path, help="Explicit predicted_target_cache_state_trace.json path.")
    parser.add_argument("--target-manifest", type=Path, help="Target profile_manifest.json path.")
    parser.add_argument(
        "--oracle-trace",
        type=Path,
        action="append",
        default=[],
        help="Explicit target Python probe trace path. Can be repeated.",
    )
    parser.add_argument("--observed-target-trace", type=Path, help="observed_target_transition_trace.json path.")
    parser.add_argument("--matrix-dir", type=Path, help="hicache_state_workflow output directory.")
    parser.add_argument("--output", type=Path, help="Primary JSON output path. Defaults depend on --mode.")
    parser.add_argument(
        "--catalog-output",
        type=Path,
        help="Output path used by --emit-catalog. Default: transition_mismatch_catalog.json.",
    )
    parser.add_argument(
        "--gate-output",
        type=Path,
        help="Matrix scoreboard output path used by --emit-gates. Default: transition_patch_gate_scoreboard.json.",
    )
    parser.add_argument("--page-key-mode", default=DEFAULT_PAGE_KEY_MODE, choices=("strip_scope", "raw"))
    parser.add_argument("--force", action="store_true", help="Rebuild existing oracle / compare artifacts.")
    parser.add_argument(
        "--emit-catalog",
        action="store_true",
        help="Emit transition mismatch catalog artifacts from compare results.",
    )
    parser.add_argument("--emit-gates", action="store_true", help="Emit operation gate artifacts from compare results.")
    parser.add_argument("--sample", type=int, default=20, help="Maximum mismatch / issue sample size.")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """CLI 入口。"""

    args = parse_args(argv)
    mode = str(args.mode)
    if mode == "model-self-check":
        prediction_paths = prediction_paths_from_args(args)
        output = resolve_output(args.output, prediction_paths.model_self_check)
        write_json(output, build_model_self_check(prediction_paths.predicted_trace, sample_limit=args.sample))
        return 0
    if mode == "extract-target-oracle":
        trace_paths, target_metadata = target_trace_paths_from_args(args)
        output = resolve_output(args.output, default_target_oracle_output(args, target_metadata))
        write_json(output, extract_target_oracle(trace_paths, target_metadata, sample_limit=args.sample))
        return 0
    if mode in {"compare-self", "compare-cross"}:
        prediction_paths = prediction_paths_from_args(args)
        observed_path = resolve_required_path(args.observed_target_trace, "--observed-target-trace")
        default_name = "transition_exactness_self.json" if mode == "compare-self" else "transition_exactness_cross.json"
        output = resolve_output(args.output, prediction_paths.prediction_dir / default_name)
        comparison = compare_prediction_to_observed(
            prediction_paths,
            observed_path,
            comparison_mode="self" if mode == "compare-self" else "cross",
            page_key_mode=args.page_key_mode,
            sample_limit=args.sample,
            include_classification_evidence=bool(args.emit_catalog),
        )
        write_json(output, comparison)
        emit_single_compare_derivatives(
            args,
            prediction_paths,
            observed_path,
            classification_entry_from_comparison(comparison),
        )
        return 0
    if mode == "compare-matrix":
        matrix_dir = resolve_required_path(args.matrix_dir, "--matrix-dir")
        output = resolve_output(args.output, matrix_dir / "transition_exactness_matrix.json")
        write_json(
            output,
            compare_transition_matrix(
                matrix_dir,
                page_key_mode=args.page_key_mode,
                force=args.force,
                sample_limit=args.sample,
                emit_catalog=args.emit_catalog,
                emit_gates=args.emit_gates,
                catalog_output=args.catalog_output,
                gate_output=args.gate_output,
                matrix_output_path=output,
            ),
        )
        return 0
    raise SystemExit(f"unsupported mode: {mode}")


def classification_entry_from_comparison(comparison: dict[str, Any]) -> dict[str, Any]:
    """从 compare 输出中读取分类 entry。"""

    entry = comparison.get("transition_classification")
    return entry if isinstance(entry, dict) else {}


def emit_single_compare_derivatives(
    args: argparse.Namespace,
    prediction_paths: PathsForPrediction,
    observed_path: Path,
    classification_entry: dict[str, Any],
) -> None:
    """按显式开关写出单格 catalog/gate 派生产物。"""

    if args.emit_catalog:
        catalog_output = resolve_output(
            args.catalog_output,
            prediction_paths.prediction_dir / "transition_mismatch_catalog.json",
        )
        catalog = build_transition_mismatch_catalog_from_entries(
            prediction_paths.prediction_dir,
            [classification_entry],
            source_matrix_path="",
            sample_limit=args.sample,
        )
        write_transition_catalog_outputs(
            prediction_paths.prediction_dir,
            catalog_output,
            catalog,
            sample_limit=args.sample,
        )
    if args.emit_gates:
        write_prediction_gate_outputs(
            prediction_paths.prediction_dir,
            observed_path,
            classification_entry,
            page_key_mode=args.page_key_mode,
            sample_limit=args.sample,
        )


def resolve_repo_path(path: Path) -> Path:
    """解析 repo 相对路径。"""

    path = path.expanduser()
    if path.is_absolute():
        return map_repo_path(path)
    return ROOT_DIR / path


def map_repo_path(path: Path) -> Path:
    """把容器内仓库前缀映射为当前 workspace。"""

    raw = str(path)
    for prefix in ("/workspace/trace-sim", "/opt/trace-sim"):
        if raw == prefix:
            return ROOT_DIR
        if raw.startswith(prefix + "/"):
            return ROOT_DIR / raw[len(prefix) + 1 :]
    return path


def resolve_required_path(path: Path | None, flag_name: str) -> Path:
    """解析必需路径参数。"""

    if path is None:
        raise SystemExit(f"missing required {flag_name}")
    return resolve_repo_path(path)


def resolve_output(path: Path | None, default: Path) -> Path:
    """解析输出路径。"""

    return resolve_repo_path(path) if path is not None else default


def load_json(path: Path) -> Any:
    """读取 JSON。"""

    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value: Any) -> None:
    """写出稳定 JSON。"""

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def prediction_paths_from_args(args: argparse.Namespace) -> PathsForPrediction:
    """从 CLI 参数解析 prediction 标准路径。"""

    if args.prediction_dir is None and args.predicted_trace is None:
        raise SystemExit("missing --prediction-dir or --predicted-trace")
    prediction_dir = (
        resolve_repo_path(args.prediction_dir)
        if args.prediction_dir
        else resolve_repo_path(args.predicted_trace).parent
    )
    predicted_trace = (
        resolve_repo_path(args.predicted_trace)
        if args.predicted_trace
        else prediction_dir / "predicted_target_cache_state_trace.json"
    )
    return PathsForPrediction(
        prediction_dir=prediction_dir,
        predicted_trace=predicted_trace,
        validation=prediction_dir / "validation.json",
        model_self_check=prediction_dir / "model_transition_self_check.json",
    )


def default_target_oracle_output(args: argparse.Namespace, target_metadata: dict[str, Any]) -> Path:
    """返回 target oracle 的缺省输出路径。"""

    if args.target_manifest is not None:
        run_dir = Path(str(target_metadata.get("target_run_dir") or "")).expanduser()
        if run_dir:
            return map_repo_path(run_dir) / "modeling" / "observed_target_transition_trace.json"
    return ROOT_DIR / "data/modeling_runs/hicache_transition_exactness/observed_target_transition_trace.json"


def target_trace_paths_from_args(args: argparse.Namespace) -> tuple[list[Path], dict[str, Any]]:
    """解析 target profile manifest 或显式 oracle trace。"""

    if args.target_manifest is not None:
        run = profile_run_from_manifest(resolve_repo_path(args.target_manifest))
        return list(run.python_probe_files), {
            "target_manifest_path": str(run.manifest_path),
            "target_run_dir": str(run.run_dir),
            "target_run_id": run.run_id,
            "target_config_id": run.config_id,
            "input_id": run.input_id,
            "input_class": run.input_class,
        }
    trace_paths = [resolve_repo_path(path) for path in args.oracle_trace]
    if not trace_paths:
        raise SystemExit("missing --target-manifest or --oracle-trace")
    return trace_paths, {
        "target_manifest_path": "",
        "target_run_dir": "",
        "target_run_id": "",
        "target_config_id": "",
        "input_id": "",
        "input_class": "",
    }


def load_predicted_trace(path: Path) -> dict[str, Any]:
    """读取 predicted state trace，并做基本结构检查。"""

    if not path.is_file():
        raise FileNotFoundError(f"missing predicted trace: {path}")
    payload = load_json(path)
    if not isinstance(payload, dict):
        raise ValueError(f"predicted trace is not a JSON object: {path}")
    records = payload.get("records")
    if not isinstance(records, list):
        payload["records"] = []
    return payload


def build_model_self_check(predicted_trace_path: Path, *, sample_limit: int) -> dict[str, Any]:
    """构建第 1 阶段模型侧 transition 自洽报告。"""

    predicted = load_predicted_trace(predicted_trace_path)
    records = predicted_records(predicted)
    schema_check = check_predicted_trace_schema(predicted, records, sample_limit=sample_limit)
    replay = replay_predicted_records(records, sample_limit=sample_limit)
    final_state = predicted_final_state(predicted)
    replay_check = compare_replay_final_state(replay, final_state, sample_limit=sample_limit)
    provenance_check = build_provenance_check(records, sample_limit=sample_limit)
    lifecycle_check = build_lifecycle_check(records, sample_limit=sample_limit)

    ready = (
        not schema_check["unknown_transition_kinds"]
        and not schema_check["missing_required_fields"]
        and replay_check["replay_final_state_match"]
        and not replay["state_constraint_violations"]
        and not lifecycle_check["unclosed_required_lifecycle"]
    )
    return {
        "schema": "trace_sim.hicache.model_transition_self_check.v1",
        "ready": ready,
        "predicted_trace_path": str(predicted_trace_path),
        "record_count": len(records),
        "schema_check": schema_check,
        "replay_check": replay_check,
        "state_constraint_check": {
            "state_constraint_violation_count": len(replay["state_constraint_violations"]),
            "state_constraint_violations": replay["state_constraint_violations"][:sample_limit],
            "ref_balance_issue_count": len(replay["ref_balance_issues"]),
            "ref_balance_issues": replay["ref_balance_issues"][:sample_limit],
            "derived_noop_transition_count": len(replay["derived_noop_transitions"]),
            "derived_noop_transitions": replay["derived_noop_transitions"][:sample_limit],
        },
        "provenance_check": provenance_check,
        "lifecycle_check": lifecycle_check,
        "replayed_final_state": replay["final_state"],
        "replayed_delta_summary": {
            "delta_row_count": len(replay["delta_rows"]),
            "delta_count_by_kind": count_rows_by_transition_kind(replay["delta_rows"]),
        },
    }


def predicted_records(predicted: dict[str, Any]) -> list[dict[str, Any]]:
    """读取 predicted trace 中的 transition records。"""

    records = predicted.get("records")
    if not isinstance(records, list):
        return []
    return [record for record in records if isinstance(record, dict)]


def predicted_final_state(predicted: dict[str, Any]) -> dict[str, Any]:
    """读取 predicted trace 中的 final_state。"""

    final_state = predicted.get("final_state")
    return final_state if isinstance(final_state, dict) else {}


def check_predicted_trace_schema(predicted: dict[str, Any], records: list[dict[str, Any]], *, sample_limit: int) -> dict[str, Any]:
    """检查 predicted transition trace 的 schema 完整性。"""

    missing_required: list[dict[str, Any]] = []
    missing_advisory: collections.Counter[str] = collections.Counter()
    unknown_kinds: collections.Counter[str] = collections.Counter()
    empty_page_mutations: list[dict[str, Any]] = []
    non_monotonic_indices: list[dict[str, Any]] = []
    previous_source_index = -1
    for ordinal, record in enumerate(records):
        kind = str(record.get("transition_kind") or "")
        if kind not in KNOWN_TRANSITION_KINDS:
            unknown_kinds[kind or "<empty>"] += 1
        missing = [field for field in CORE_RECORD_FIELDS if missing_core_field(record, field)]
        if missing and len(missing_required) < sample_limit:
            missing_required.append({"ordinal": ordinal, "missing_fields": missing, "transition_kind": kind})
        for field in ADVISORY_RECORD_FIELDS:
            if missing_core_field(record, field):
                missing_advisory[field] += 1
        pages = record_pages(record)
        if kind and kind != "increment_hit_count" and not pages:
            empty_page_mutations.append({"ordinal": ordinal, "transition_kind": kind, "source_event_index": record.get("source_event_index")})
        source_index = optional_int(record.get("source_event_index"), -1)
        if source_index >= 0 and previous_source_index > source_index:
            non_monotonic_indices.append({"ordinal": ordinal, "previous_source_event_index": previous_source_index, "source_event_index": source_index})
        if source_index >= 0:
            previous_source_index = max(previous_source_index, source_index)
    return {
        "schema_name": predicted.get("schema"),
        "record_count": len(records),
        "unknown_transition_kinds": dict(sorted(unknown_kinds.items())),
        "missing_required_field_count": sum(len(item["missing_fields"]) for item in missing_required),
        "missing_required_fields": missing_required,
        "missing_advisory_field_counts": dict(sorted(missing_advisory.items())),
        "transition_id_present": all("transition_id" in record for record in records),
        "transition_id_note": "predicted_target_cache_state_trace.v1 may omit C++ transition_id; replay uses record ordinal when absent.",
        "empty_page_mutation_count": len(empty_page_mutations),
        "empty_page_mutations": empty_page_mutations[:sample_limit],
        "non_monotonic_source_event_index_count": len(non_monotonic_indices),
        "non_monotonic_source_event_indices": non_monotonic_indices[:sample_limit],
    }


def missing_core_field(record: dict[str, Any], field: str) -> bool:
    """判断 record 字段是否缺失。"""

    if field not in record:
        return True
    value = record.get(field)
    if field == "target_page_set":
        return not isinstance(value, list)
    return value is None


def replay_predicted_records(records: list[dict[str, Any]], *, sample_limit: int) -> dict[str, Any]:
    """按 transition 顺序回放模型侧 active state。"""

    state: dict[str, set[str]] = {key: set() for key in ACTIVE_STATE_KEYS}
    page_hit_counts: collections.Counter[str] = collections.Counter()
    ref_counts: collections.Counter[str] = collections.Counter()
    delta_rows: list[dict[str, Any]] = []
    violations: list[dict[str, Any]] = []
    ref_issues: list[dict[str, Any]] = []
    noop_rows: list[dict[str, Any]] = []

    for ordinal, record in enumerate(records):
        kind = str(record.get("transition_kind") or "")
        pages = record_pages(record)
        before_delta_count = len(delta_rows)
        if kind in {"add_l1_residency", "restore_l1_residency", "promote_visible_prefix_to_l1"}:
            add_pages(state, delta_rows, "l1_resident_pages", pages, record, ordinal)
            remove_pages(state, delta_rows, "evicted_pages", pages, record, ordinal, strict=False)
        elif kind == "mark_dirty":
            add_pages(state, delta_rows, "dirty_pages", pages, record, ordinal)
        elif kind in {"commit_host_storage_backup", "commit_host_backup"}:
            add_pages(state, delta_rows, "l2_resident_pages", pages, record, ordinal)
            add_pages(state, delta_rows, "backuped_pages", pages, record, ordinal)
            if kind == "commit_host_storage_backup":
                add_pages(state, delta_rows, "l3_resident_pages", pages, record, ordinal)
            remove_pages(state, delta_rows, "dirty_pages", pages, record, ordinal, strict=False)
        elif kind == "apply_prefetch_host_visibility":
            add_pages(state, delta_rows, "l2_resident_pages", pages, record, ordinal)
            add_pages(state, delta_rows, "backuped_pages", pages, record, ordinal)
            add_pages(state, delta_rows, "l3_resident_pages", pages, record, ordinal)
            add_pages(state, delta_rows, "evicted_pages", [page for page in pages if page not in state["l1_resident_pages"]], record, ordinal)
        elif kind == "evict_l1_node":
            remove_pages(state, delta_rows, "l1_resident_pages", pages, record, ordinal, strict=True, violations=violations)
            remove_pages(state, delta_rows, "dirty_pages", pages, record, ordinal, strict=False)
            backed_pages = [page for page in pages if page in state["l2_resident_pages"] or page in state["backuped_pages"]]
            add_pages(state, delta_rows, "evicted_pages", backed_pages, record, ordinal)
        elif kind == "evict_host_node":
            for key in ("l2_resident_pages", "l3_resident_pages", "backuped_pages", "evicted_pages", "locked_pages"):
                remove_pages(state, delta_rows, key, pages, record, ordinal, strict=False)
            for page in pages:
                ref_counts.pop(page, None)
        elif kind == "prefetch_planned":
            add_pages(state, delta_rows, "prefetch_planned_pages", pages, record, ordinal)
        elif kind == "prefetch_ready":
            add_pages(state, delta_rows, "prefetch_ready_pages", pages, record, ordinal)
        elif kind in {"prefetch_revoked", "prefetch_suppressed"}:
            add_pages(state, delta_rows, "prefetch_suppressed_pages", pages, record, ordinal)
        elif kind == "prefetch_timeout_incomplete":
            add_pages(state, delta_rows, "prefetch_late_pages", pages, record, ordinal)
        elif kind == "prefetch_terminated":
            pass
        elif kind == "increment_hit_count":
            for page in pages:
                page_hit_counts[page] += 1
        elif kind == "enqueue_writeback":
            add_pages(state, delta_rows, "pending_writeback_pages", pages, record, ordinal)
            acquire_refs(state, delta_rows, ref_counts, pages, record, ordinal)
        elif kind in LOCK_ACQUIRE_KINDS:
            acquire_refs(state, delta_rows, ref_counts, pages, record, ordinal)
        elif kind in LOCK_RELEASE_BY_KIND:
            if kind in {"complete_writeback", "cancel_writeback"}:
                remove_pages(state, delta_rows, "pending_writeback_pages", pages, record, ordinal, strict=False)
            release_refs(state, delta_rows, ref_counts, pages, record, ordinal, ref_issues)
        elif kind in {"enqueue_storage_backup", "enqueue_write_through_backup", "enqueue_loadback", "complete_storage_backup", "complete_loadback"}:
            pass
        elif kind:
            violations.append(
                {
                    "kind": "unknown_transition_kind",
                    "ordinal": ordinal,
                    "transition_kind": kind,
                    "source_event_index": record.get("source_event_index"),
                }
            )
        if len(delta_rows) == before_delta_count and pages and kind not in {
            "increment_hit_count",
            "complete_storage_backup",
            "complete_loadback",
            "complete_writeback",
            "cancel_writeback",
            "prefetch_terminated",
        }:
            noop_rows.append(
                {
                    "ordinal": ordinal,
                    "transition_kind": kind,
                    "source_event_index": record.get("source_event_index"),
                    "page_count": len(pages),
                }
            )

    final_state = {key: sorted(values) for key, values in state.items()}
    final_state["page_hit_counts"] = dict(sorted(page_hit_counts.items()))
    return {
        "final_state": final_state,
        "delta_rows": delta_rows,
        "state_constraint_violations": violations[: max(sample_limit, len(violations))],
        "ref_balance_issues": ref_issues[: max(sample_limit, len(ref_issues))],
        "derived_noop_transitions": noop_rows[: max(sample_limit, len(noop_rows))],
    }


def record_pages(record: dict[str, Any]) -> list[str]:
    """读取 transition record 的 page 集合。"""

    pages = record.get("target_page_set")
    if not isinstance(pages, list):
        return []
    return [str(page) for page in pages if page is not None]


def add_pages(
    state: dict[str, set[str]],
    delta_rows: list[dict[str, Any]],
    key: str,
    pages: list[str],
    record: dict[str, Any],
    ordinal: int,
) -> None:
    """向某个状态集合添加页，并记录实际 delta。"""

    changed = [page for page in pages if page not in state[key]]
    if not changed:
        return
    state[key].update(changed)
    emit_delta(delta_rows, key, True, changed, record, ordinal)


def remove_pages(
    state: dict[str, set[str]],
    delta_rows: list[dict[str, Any]],
    key: str,
    pages: list[str],
    record: dict[str, Any],
    ordinal: int,
    *,
    strict: bool,
    violations: list[dict[str, Any]] | None = None,
) -> None:
    """从某个状态集合删除页，并记录实际 delta。"""

    missing = [page for page in pages if page not in state[key]]
    if strict and missing and violations is not None:
        violations.append(
            {
                "kind": "remove_missing_page",
                "state_key": key,
                "ordinal": ordinal,
                "transition_kind": record.get("transition_kind"),
                "source_event_index": record.get("source_event_index"),
                "missing_pages_sample": missing[:8],
                "missing_count": len(missing),
            }
        )
    changed = [page for page in pages if page in state[key]]
    if not changed:
        return
    state[key].difference_update(changed)
    emit_delta(delta_rows, key, False, changed, record, ordinal)


def acquire_refs(
    state: dict[str, set[str]],
    delta_rows: list[dict[str, Any]],
    ref_counts: collections.Counter[str],
    pages: list[str],
    record: dict[str, Any],
    ordinal: int,
) -> None:
    """按 page 维护简化 ref 计数。"""

    newly_locked: list[str] = []
    for page in pages:
        ref_counts[page] += 1
        if ref_counts[page] == 1 and page not in state["locked_pages"]:
            state["locked_pages"].add(page)
            newly_locked.append(page)
    if newly_locked:
        emit_delta(delta_rows, "locked_pages", True, newly_locked, record, ordinal)


def release_refs(
    state: dict[str, set[str]],
    delta_rows: list[dict[str, Any]],
    ref_counts: collections.Counter[str],
    pages: list[str],
    record: dict[str, Any],
    ordinal: int,
    ref_issues: list[dict[str, Any]],
) -> None:
    """释放简化 ref 计数，并报告负计数风险。"""

    cleared: list[str] = []
    missing: list[str] = []
    for page in pages:
        if ref_counts[page] <= 0:
            missing.append(page)
            ref_counts.pop(page, None)
            continue
        ref_counts[page] -= 1
        if ref_counts[page] <= 0:
            ref_counts.pop(page, None)
            if page in state["locked_pages"]:
                state["locked_pages"].remove(page)
                cleared.append(page)
    if missing:
        ref_issues.append(
            {
                "kind": "release_ref_without_replay_acquire",
                "ordinal": ordinal,
                "transition_kind": record.get("transition_kind"),
                "source_event_index": record.get("source_event_index"),
                "missing_pages_sample": missing[:8],
                "missing_count": len(missing),
            }
        )
    if cleared:
        emit_delta(delta_rows, "locked_pages", False, cleared, record, ordinal)


def emit_delta(
    delta_rows: list[dict[str, Any]],
    state_key: str,
    is_add: bool,
    pages: list[str],
    record: dict[str, Any],
    ordinal: int,
) -> None:
    """记录 replay 产生的可比较状态 delta。"""

    kinds = STATE_DELTA_KINDS.get(state_key)
    if kinds is None or not pages:
        return
    transition_kind = kinds[0] if is_add else kinds[1]
    delta_rows.append(
        {
            "transition_ordinal": ordinal,
            "source_transition_kind": record.get("transition_kind"),
            "transition_kind": transition_kind,
            "state_key": state_key,
            "pages": sorted(set(pages)),
            "cache_scope": record.get("cache_scope") or "",
            "request_id": record.get("request_id") or "",
            "operation_id": record.get("operation_id") or "",
            "source_event_index": record.get("source_event_index"),
            "source_event_name": record.get("source_event_name") or "",
            "event_base_name": record.get("event_base_name") or "",
            "ts": record.get("ts"),
        }
    )


def compare_replay_final_state(replay: dict[str, Any], final_state: dict[str, Any], *, sample_limit: int) -> dict[str, Any]:
    """比较 replay final state 和模型 summary final state。"""

    replay_final = replay["final_state"]
    diffs: dict[str, Any] = {}
    strict_active_sets_match = True
    hard_active_sets_match = True
    for key in ACTIVE_STATE_KEYS:
        model_pages = normalize_page_set(final_state.get(key, []), "raw")
        replay_pages = normalize_page_set(replay_final.get(key, []), "raw")
        missing = sorted(model_pages - replay_pages)
        extra = sorted(replay_pages - model_pages)
        if missing or extra:
            strict_active_sets_match = False
            if key in SELF_CHECK_HARD_STATE_KEYS:
                hard_active_sets_match = False
        diffs[key] = {
            "match": not missing and not extra,
            "model_count": len(model_pages),
            "replayed_count": len(replay_pages),
            "missing_in_replay": missing[:sample_limit],
            "extra_in_replay": extra[:sample_limit],
            "missing_count": len(missing),
            "extra_count": len(extra),
        }
    model_hits = {str(key): int(value) for key, value in (final_state.get("page_hit_counts") or {}).items()} if isinstance(final_state.get("page_hit_counts"), dict) else {}
    replay_hits = {str(key): int(value) for key, value in (replay_final.get("page_hit_counts") or {}).items()} if isinstance(replay_final.get("page_hit_counts"), dict) else {}
    hit_mismatch = compare_counter_dicts(model_hits, replay_hits, sample_limit=sample_limit)
    return {
        "replay_final_state_match": hard_active_sets_match,
        "active_set_replay_match": hard_active_sets_match,
        "strict_active_set_replay_match": strict_active_sets_match,
        "strict_replay_final_state_match": strict_active_sets_match and hit_mismatch["match"],
        "page_hit_counts_match": hit_mismatch["match"],
        "replayed_final_state_counts": state_counts(replay_final),
        "model_final_state_counts": state_counts(final_state),
        "sets_diff_by_tier": diffs,
        "page_hit_counts_diff": hit_mismatch,
        "unreplayed_state_transition_count": 0,
        "advisory_replay_state_keys": {
            "locked_pages": "predicted transition trace does not currently expose every source_actual lock/ref or prefetch anchor protection mutation; strict mismatch is reported but stable state exactness does not gate on it.",
            "page_hit_counts": "hit count is diagnostic metadata and is reported separately from active-state replay.",
        },
    }


def normalize_page_set(value: Any, page_key_mode: str) -> set[str]:
    """把页面列表归一化成集合。"""

    if not isinstance(value, list):
        return set()
    return {normalize_hicache_page_key(page, page_key_mode) for page in value if page is not None}


def state_counts(state: dict[str, Any]) -> dict[str, int]:
    """统计 state 集合字段规模。"""

    counts: dict[str, int] = {}
    for key, value in sorted(state.items()):
        if isinstance(value, list):
            counts[key] = len({str(item) for item in value if item is not None})
        elif isinstance(value, dict) and key == "page_hit_counts":
            counts[key] = len(value)
    return counts


def compare_counter_dicts(expected: dict[str, int], actual: dict[str, int], *, sample_limit: int) -> dict[str, Any]:
    """比较两个 page counter 字典。"""

    mismatches: list[dict[str, Any]] = []
    for key in sorted(set(expected) | set(actual)):
        left = expected.get(key, 0)
        right = actual.get(key, 0)
        if left != right:
            mismatches.append({"page": key, "model_count": left, "replayed_count": right})
    return {
        "match": not mismatches,
        "mismatch_count": len(mismatches),
        "mismatches": mismatches[:sample_limit],
    }


def build_provenance_check(records: list[dict[str, Any]], *, sample_limit: int) -> dict[str, Any]:
    """检查 transition 是否携带基本来源字段。"""

    without_source = []
    without_policy_hint = []
    by_source_event = collections.Counter()
    for ordinal, record in enumerate(records):
        source_index = record.get("source_event_index")
        source_name = str(record.get("source_event_name") or "")
        if source_index is None or not source_name:
            without_source.append({"ordinal": ordinal, "transition_kind": record.get("transition_kind"), "source_event_index": source_index})
        by_source_event[source_name] += 1
        if not str(record.get("decision_reason") or ""):
            without_policy_hint.append({"ordinal": ordinal, "transition_kind": record.get("transition_kind")})
    return {
        "transitions_without_source_fact_count": len(without_source),
        "transitions_without_source_fact": without_source[:sample_limit],
        "transitions_without_policy_decision_hint_count": len(without_policy_hint),
        "transitions_without_policy_decision_hint": without_policy_hint[:sample_limit],
        "transition_count_by_source_event": dict(sorted(by_source_event.items())),
    }


def build_lifecycle_check(records: list[dict[str, Any]], *, sample_limit: int) -> dict[str, Any]:
    """检查粗粒度 lifecycle 是否闭合。"""

    prefetch_by_request: dict[str, collections.Counter[str]] = collections.defaultdict(collections.Counter)
    writeback_pending: collections.Counter[str] = collections.Counter()
    storage_pending: collections.Counter[str] = collections.Counter()
    loadback_pending: collections.Counter[str] = collections.Counter()
    write_through_pending: collections.Counter[str] = collections.Counter()
    for record in records:
        kind = str(record.get("transition_kind") or "")
        request_key = transition_request_key(record)
        if kind.startswith("prefetch_") or kind == "apply_prefetch_host_visibility":
            prefetch_by_request[request_key][kind] += 1
        write_through_key = str(record.get("cache_scope") or "")
        if kind == "enqueue_writeback":
            writeback_pending[request_key] += 1
        elif kind in {"complete_writeback", "cancel_writeback"} and writeback_pending[request_key] > 0:
            writeback_pending[request_key] -= 1
        if kind == "enqueue_storage_backup":
            storage_pending[request_key] += 1
        elif kind == "complete_storage_backup" and storage_pending[request_key] > 0:
            storage_pending[request_key] -= 1
        if kind == "enqueue_loadback":
            loadback_pending[request_key] += 1
        elif kind == "complete_loadback" and loadback_pending[request_key] > 0:
            loadback_pending[request_key] -= 1
        if kind == "enqueue_write_through_backup":
            # write-through backup ref 是 scope 级 pending 队列；C++ 会在后续事实上 drain，
            # completion 的 request_id 可能来自触发 drain 的新 fact，不能按 request 级闭合。
            write_through_pending[write_through_key] += 1
        elif kind == "complete_write_through_backup" and write_through_pending[write_through_key] > 0:
            write_through_pending[write_through_key] -= 1

    unclosed: list[dict[str, Any]] = []
    advisory_open: list[dict[str, Any]] = []
    for request_key, counts in sorted(prefetch_by_request.items()):
        planned = counts.get("prefetch_planned", 0)
        terminal = (
            counts.get("prefetch_ready", 0)
            + counts.get("prefetch_revoked", 0)
            + counts.get("prefetch_suppressed", 0)
            + counts.get("prefetch_terminated", 0)
            + counts.get("prefetch_timeout_incomplete", 0)
        )
        if terminal < planned:
            unclosed.append({"lifecycle": "prefetch", "request_key": request_key, "planned": planned, "terminal": terminal, "counts": dict(counts)})
    for lifecycle, pending in (
        ("writeback", writeback_pending),
        ("storage_backup", storage_pending),
        ("loadback", loadback_pending),
    ):
        for request_key, count in sorted(pending.items()):
            if count > 0:
                unclosed.append({"lifecycle": lifecycle, "request_key": request_key, "pending_count": count})
    for request_key, count in sorted(write_through_pending.items()):
        if count > 0:
            advisory_open.append(
                {
                    "lifecycle": "write_through_backup",
                    "request_key": request_key,
                    "pending_count": count,
                    "reason": "write-through backup may intentionally remain as pending lock release boundary in final state.",
                }
            )
    return {
        "unclosed_required_lifecycle_count": len(unclosed),
        "unclosed_required_lifecycle": unclosed[:sample_limit],
        "advisory_open_lifecycle_count": len(advisory_open),
        "advisory_open_lifecycle": advisory_open[:sample_limit],
        "prefetch_request_count": len(prefetch_by_request),
    }


def transition_request_key(record: dict[str, Any]) -> str:
    """生成 run-local request / operation 归因 key。"""

    return ":".join(
        [
            str(record.get("cache_scope") or ""),
            str(record.get("request_id") or ""),
            str(record.get("operation_id") or ""),
        ]
    )


def extract_target_oracle(trace_paths: list[Path], target_metadata: dict[str, Any], *, sample_limit: int) -> dict[str, Any]:
    """构建第 2 阶段 target-side observed transition oracle。"""

    snapshots = extract_hicache_state_snapshots(trace_paths)
    timeline_oracle = build_oracle_timeline_deltas(snapshots, set(SNAPSHOT_VISIBLE_STATE_KEYS))
    observed_transitions = observed_transitions_from_snapshot_rows(timeline_oracle["rows"])
    observed_operations, event_status = extract_observed_operations(trace_paths, sample_limit=sample_limit)
    final_state = latest_derived_state(snapshots)
    visible_keys = sorted(timeline_visible_keys_from_snapshots(snapshots))
    unsupported_keys = sorted(set(ACTIVE_STATE_KEYS) - set(SNAPSHOT_VISIBLE_STATE_KEYS))
    ready = bool(snapshots) and (bool(observed_operations) or bool(observed_transitions))
    return {
        "schema": "trace_sim.hicache.observed_target_transition_trace.v1",
        "oracle_ready": ready,
        **target_metadata,
        "oracle_trace_files": [str(path) for path in trace_paths],
        "observability_summary": {
            "state_snapshot_count": len(snapshots),
            "completed_state_snapshot_count": sum(1 for row in snapshots if snapshot_is_completed_state(row)),
            "snapshot_delta_row_count": len(timeline_oracle["rows"]),
            "observed_operation_count": len(observed_operations),
            "observed_transition_count": len(observed_transitions),
            "visible_state_keys": visible_keys,
            "unsupported_or_unobservable_state_keys": unsupported_keys,
            "trace_load_status": event_status,
            "object_group_count": timeline_oracle.get("object_group_count", 0),
            "snapshot_count_with_object_id": timeline_oracle.get("snapshot_count_with_object_id", 0),
            "snapshot_count_without_object_id": timeline_oracle.get("snapshot_count_without_object_id", 0),
        },
        "observed_transitions": observed_transitions,
        "observed_operations": observed_operations,
        "snapshot_delta_rows": timeline_oracle["rows"],
        "final_state": final_state,
        "final_state_counts": state_counts(final_state),
        "unsupported_or_unobservable_state_keys": unsupported_keys,
        "notes": [
            "snapshot_delta_rows are derived from validation-only state_snapshot timeline and are labels only.",
            "observed_operations are source_actual/timing evidence from the target run and must not be model input.",
            "L3 and prefetch internal sets are model-side state unless a future probe exposes them directly.",
        ],
        "samples": {
            "observed_transitions": observed_transitions[:sample_limit],
            "observed_operations": observed_operations[:sample_limit],
        },
    }


def observed_transitions_from_snapshot_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """把 snapshot delta rows 规整成 observed transition rows。"""

    result: list[dict[str, Any]] = []
    for index, row in enumerate(rows):
        pages = [str(page) for page in row.get("pages", []) if page is not None] if isinstance(row.get("pages"), list) else []
        result.append(
            {
                "observed_transition_id": f"snapshot_delta:{index}",
                "operation_kind": "snapshot_state_delta",
                "state_delta_kind": row.get("transition_kind") or "",
                "transition_kind": row.get("transition_kind") or "",
                "pages": pages,
                "cache_scope": row.get("cache_scope") or "",
                "request_id": row.get("request_id") or "",
                "operation_id": row.get("operation_id") or "",
                "canonical_request_key": canonical_request_key_from_row(row),
                "event_base_name": row.get("event_base_name") or "",
                "source_event_name": row.get("source_event_name") or "",
                "ts": row.get("ts"),
                "evidence_class": "oracle_state_snapshot_delta",
                "evidence_event_indices": [row.get("event_key") or ""],
                "confidence": "snapshot_delta",
            }
        )
    return result


def extract_observed_operations(trace_paths: list[Path], *, sample_limit: int) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """从 target full probe 中抽取 source_actual/timing operation evidence。"""

    operations: list[dict[str, Any]] = []
    statuses: list[dict[str, Any]] = []
    for path in trace_paths:
        events, status = load_chrome_trace_events(path, auto_repair=True)
        statuses.append(status.to_dict())
        for ordinal, event in enumerate(events):
            args = event.get("args") if isinstance(event.get("args"), dict) else {}
            fact = parse_fact_or_none(args)
            if fact is None or fact.fact_class not in {"source_actual", "timing_observation"}:
                continue
            role = fact.role
            event_kind = str(args.get("event_kind") or event.get("name") or "")
            operations.append(
                {
                    "observed_operation_id": f"{path.name}:{ordinal}",
                    "operation_kind": observed_operation_kind(role, event_kind),
                    "fact_role": role,
                    "event_kind": event_kind,
                    "event_name": event.get("name") or "",
                    "fact_class": fact.fact_class,
                    "cache_scope": args.get("cache_scope") or "",
                    "request_id": args.get("request_id") or "",
                    "operation_id": args.get("operation_id") or "",
                    "canonical_request_key": canonical_request_key_from_row(args),
                    "pages": pages_from_operation_args(args),
                    "ts": event.get("ts"),
                    "dur": event.get("dur"),
                    "trace_path": str(path),
                    "confidence": "source_actual" if fact.fact_class == "source_actual" else "timing",
                }
            )
    return operations, statuses


def observed_operation_kind(role: str, event_kind: str) -> str:
    """把 probe role 归一化为 transition patch gate 使用的 operation kind。"""

    if role in OBSERVED_ROLE_TO_OPERATION_KIND:
        return OBSERVED_ROLE_TO_OPERATION_KIND[role]
    if "prefetch" in role or "prefetch" in event_kind:
        return "prefetch"
    if "writeback" in role or "writeback" in event_kind:
        return "write_back_flush"
    if "write" in role or "write" in event_kind:
        return "write_through_backup"
    if "lock" in role or "ref" in role:
        return "lock_ref"
    if "capacity" in role or "evict" in role:
        return "capacity"
    return role or event_kind or "unknown"


def pages_from_operation_args(args: dict[str, Any]) -> list[str]:
    """从 source_actual args 中尽力提取 page/hash 列表。"""

    candidates = (
        "pages",
        "hash_pages",
        "hash_value",
        "page_hashes",
        "target_page_set",
        "evicted_pages",
        "loaded_pages",
        "host_pages",
        "device_pages",
        "planned_pages",
        "hit_pages",
        "queue_snapshot",
    )
    pages: list[str] = []
    for key in candidates:
        pages.extend(flatten_page_like_values(args.get(key)))
    return sorted(set(pages))


def flatten_page_like_values(value: Any) -> list[str]:
    """宽松展开可能包含 page/hash 的结构。"""

    if value is None:
        return []
    if isinstance(value, str):
        stripped = value.strip()
        if not stripped:
            return []
        if stripped.startswith("[") or stripped.startswith("{"):
            try:
                return flatten_page_like_values(json.loads(stripped))
            except json.JSONDecodeError:
                return [stripped]
        return [stripped] if looks_like_page_key(stripped) else []
    if isinstance(value, (int, float)):
        return [str(value)]
    if isinstance(value, list):
        pages: list[str] = []
        for item in value:
            pages.extend(flatten_page_like_values(item))
        return pages
    if isinstance(value, dict):
        pages: list[str] = []
        for key, item in value.items():
            if "page" in str(key) or "hash" in str(key):
                pages.extend(flatten_page_like_values(item))
        return pages
    return []


def looks_like_page_key(value: str) -> bool:
    """判断字符串是否像 page/hash 标识。"""

    return "|" in value or len(value) >= 16


def timeline_visible_keys_from_snapshots(snapshots: list[dict[str, Any]]) -> set[str]:
    """统计 target snapshot 实际暴露过的状态 key。"""

    visible: set[str] = set()
    for row in snapshots:
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
            continue
        for key, value in derived_hicache_state_from_snapshot(snapshot).items():
            if isinstance(value, list):
                visible.add(str(key))
    return visible


def compare_prediction_to_observed(
    prediction_paths: PathsForPrediction,
    observed_target_trace_path: Path,
    *,
    comparison_mode: str,
    page_key_mode: str,
    sample_limit: int,
    force_self_check: bool = False,
    context: dict[str, Any] | None = None,
    include_classification_evidence: bool = False,
) -> dict[str, Any]:
    """构建单格 transition exactness 对比，并同步生成分类与 patch gate 字段。"""

    self_check_path = prediction_paths.model_self_check
    if self_check_path.is_file() and not force_self_check:
        self_check = load_json(self_check_path)
    else:
        self_check = build_model_self_check(prediction_paths.predicted_trace, sample_limit=sample_limit)
        write_json(self_check_path, self_check)

    observed = load_json(observed_target_trace_path)
    predicted = load_predicted_trace(prediction_paths.predicted_trace)
    replay = replay_predicted_records(predicted_records(predicted), sample_limit=sample_limit)
    model_rows = comparable_model_delta_rows(replay["delta_rows"], page_key_mode)
    observed_rows = comparable_observed_delta_rows(observed.get("snapshot_delta_rows", []), page_key_mode)
    final_state = final_state_comparison(predicted_final_state(predicted), observed.get("final_state", {}), page_key_mode=page_key_mode, sample_limit=sample_limit)
    count_match = compare_transition_kind_counts(model_rows, observed_rows)
    lifecycle_match = compare_delta_multisets(model_rows, observed_rows, sample_limit=sample_limit)
    validation_summary = load_validation_summary(prediction_paths.validation)
    ready = (
        bool(self_check.get("ready"))
        and bool(observed.get("oracle_ready"))
        and final_state["match"]
        and bool(model_rows or observed_rows)
    )
    exact = ready and count_match["match"] and lifecycle_match["match"]
    result = {
        "schema": f"trace_sim.hicache.transition_exactness_{comparison_mode}.v1",
        "comparison_mode": comparison_mode,
        "ready": ready,
        "exact": exact,
        "prediction_dir": str(prediction_paths.prediction_dir),
        "predicted_trace_path": str(prediction_paths.predicted_trace),
        "observed_target_trace_path": str(observed_target_trace_path),
        "model_transition_self_check_path": str(self_check_path),
        "model_transition_self_check_ready": bool(self_check.get("ready")),
        "oracle_ready": bool(observed.get("oracle_ready")),
        "target_run_id": observed.get("target_run_id", ""),
        "target_config_id": observed.get("target_config_id", ""),
        "input_id": observed.get("input_id", ""),
        "validation_final_state_match": validation_summary.get("final_state_match"),
        "final_state_exact": final_state["match"],
        "transition_count_exact": count_match["match"],
        "page_lifecycle_multiset_exact": lifecycle_match["match"],
        "final_state_comparison": final_state,
        "transition_count_comparison": count_match,
        "page_lifecycle_multiset_comparison": lifecycle_match,
        "model_delta_count_by_kind": count_rows_by_transition_kind(model_rows),
        "observed_delta_count_by_kind": count_rows_by_transition_kind(observed_rows),
        "unsupported_or_unobservable_state_keys": observed.get("unsupported_or_unobservable_state_keys", []),
        "ignored_transition_state_keys": {
            "locked_pages": "lock/ref transient is observed through source_actual evidence, but it is not a state-model fact; final locked state remains checked through final-state exactness.",
        },
        "failure_classification": classify_transition_comparison_failure(self_check, observed, final_state, count_match, lifecycle_match),
        "notes": [
            "Transition count and page lifecycle checks compare normalized state-delta rows, not raw C++ transition names.",
            "Snapshot timeline is validation evidence only; source_actual is not consumed as model input.",
            "Cross mode uses page-key normalization and transition/page multiset; raw timestamp alignment is intentionally not required.",
        ],
    }
    classification_entry = build_transition_classification_entry(
        context or comparison_context_from_prediction(prediction_paths, observed_target_trace_path, result),
        result,
        load_hicache_summary(prediction_paths.prediction_dir / "model_summary.json"),
        observed_target_trace_path,
        page_key_mode=page_key_mode,
        sample_limit=sample_limit,
        include_evidence=include_classification_evidence,
    )
    result.update(compare_result_classification_fields(classification_entry))
    result["transition_classification"] = classification_entry
    return result


def load_validation_summary(path: Path) -> dict[str, Any]:
    """读取 validation.json 中与 transition exactness 相关的摘要。"""

    if not path.is_file():
        return {}
    payload = load_json(path)
    hicache = payload.get("hicache_state") if isinstance(payload.get("hicache_state"), dict) else {}
    return {
        "validation_ready": payload.get("validation_ready"),
        "final_state_match": hicache.get("final_state_match"),
        "raw_final_state_match": hicache.get("raw_final_state_match"),
        "sets_diff_by_tier": hicache.get("sets_diff_by_tier", {}),
    }


def comparable_model_delta_rows(rows: list[dict[str, Any]], page_key_mode: str) -> list[dict[str, Any]]:
    """筛出 snapshot 可见状态的模型 replay delta。"""

    visible_delta_kinds = {kind for state_key in TRANSITION_COMPARABLE_STATE_KEYS for kind in STATE_DELTA_KINDS[state_key]}
    return normalize_delta_rows_to_union_timeline([row for row in rows if str(row.get("transition_kind") or "") in visible_delta_kinds], page_key_mode)


def comparable_observed_delta_rows(rows: Any, page_key_mode: str) -> list[dict[str, Any]]:
    """规整 observed snapshot delta rows。"""

    if not isinstance(rows, list):
        return []
    visible_delta_kinds = {kind for state_key in TRANSITION_COMPARABLE_STATE_KEYS for kind in STATE_DELTA_KINDS[state_key]}
    return normalize_delta_rows_to_union_timeline(
        [row for row in rows if isinstance(row, dict) and str(row.get("transition_kind") or "") in visible_delta_kinds],
        page_key_mode,
    )


def normalize_delta_rows(rows: list[dict[str, Any]], page_key_mode: str) -> list[dict[str, Any]]:
    """归一化 delta rows 的 page key。"""

    result: list[dict[str, Any]] = []
    for row in rows:
        pages = row.get("pages")
        if not isinstance(pages, list):
            continue
        result.append(
            {
                **row,
                "pages": sorted({normalize_hicache_page_key(page, page_key_mode) for page in pages if page is not None}),
            }
        )
    return result


def normalize_delta_rows_to_union_timeline(rows: list[dict[str, Any]], page_key_mode: str) -> list[dict[str, Any]]:
    """按归一化 page key 重建全局 union 口径的 delta rows。

    C++ predicted trace 保留 cache scope；state oracle 的 timeline delta 与 final-state
    validation 一样按对象 union 后比较。strip_scope 后如果两个 scope 同时持有同一
    page，第二次 add 不应算成全局可见状态变化；只有计数 0->1 或 1->0 才 emit。
    """

    counts: collections.Counter[tuple[str, str]] = collections.Counter()
    result: list[dict[str, Any]] = []
    for row in normalize_delta_rows(rows, page_key_mode):
        kind = str(row.get("transition_kind") or "")
        state_key, direction = state_effect_from_delta_kind(kind)
        if not state_key or direction == 0:
            result.append(row)
            continue
        changed: list[str] = []
        for page in row.get("pages", []):
            key = (state_key, str(page))
            before = counts[key]
            after = max(0, before + direction)
            if after == 0:
                counts.pop(key, None)
            else:
                counts[key] = after
            if direction > 0 and before == 0 and after > 0:
                changed.append(str(page))
            elif direction < 0 and before > 0 and after == 0:
                changed.append(str(page))
        if changed:
            result.append({**row, "state_key": state_key, "pages": sorted(set(changed))})
    return result


def state_effect_from_delta_kind(kind: str) -> tuple[str, int]:
    """从 delta kind 反推 state key 和方向。"""

    for state_key, (add_kind, remove_kind) in STATE_DELTA_KINDS.items():
        if kind == add_kind:
            return state_key, 1
        if kind == remove_kind:
            return state_key, -1
    return "", 0


def final_state_comparison(model_final: dict[str, Any], observed_final: dict[str, Any], *, page_key_mode: str, sample_limit: int) -> dict[str, Any]:
    """比较 model/oracle final state 的 snapshot 可见字段。"""

    normalized_model = normalize_hicache_state_for_oracle_compare(model_final, page_key_mode)
    normalized_observed = normalize_hicache_state_for_oracle_compare(observed_final, page_key_mode)
    diffs: dict[str, Any] = {}
    match = True
    for key in SNAPSHOT_VISIBLE_STATE_KEYS:
        model_pages = set(str(page) for page in normalized_model.get(key, []) if page is not None) if isinstance(normalized_model.get(key), list) else set()
        observed_pages = set(str(page) for page in normalized_observed.get(key, []) if page is not None) if isinstance(normalized_observed.get(key), list) else set()
        missing = sorted(observed_pages - model_pages)
        extra = sorted(model_pages - observed_pages)
        if missing or extra:
            match = False
        diffs[key] = {
            "match": not missing and not extra,
            "model_count": len(model_pages),
            "observed_count": len(observed_pages),
            "missing_in_model_count": len(missing),
            "extra_in_model_count": len(extra),
            "missing_in_model": missing[:sample_limit],
            "extra_in_model": extra[:sample_limit],
        }
    return {
        "match": match,
        "model_final_state_counts": state_counts(normalized_model),
        "observed_final_state_counts": state_counts(normalized_observed),
        "sets_diff_by_tier": diffs,
    }


def compare_transition_kind_counts(model_rows: list[dict[str, Any]], observed_rows: list[dict[str, Any]]) -> dict[str, Any]:
    """比较 transition kind 触达页数。"""

    model_counts = count_rows_by_transition_kind(model_rows)
    observed_counts = count_rows_by_transition_kind(observed_rows)
    by_kind: dict[str, Any] = {}
    match = True
    for kind in sorted(set(model_counts) | set(observed_counts)):
        model_count = int(model_counts.get(kind, 0))
        observed_count = int(observed_counts.get(kind, 0))
        if model_count != observed_count:
            match = False
        by_kind[kind] = {
            "match": model_count == observed_count,
            "model_count": model_count,
            "observed_count": observed_count,
            "missing_in_model": max(observed_count - model_count, 0),
            "extra_in_model": max(model_count - observed_count, 0),
        }
    return {"match": match, "by_kind": by_kind}


def compare_delta_multisets(model_rows: list[dict[str, Any]], observed_rows: list[dict[str, Any]], *, sample_limit: int) -> dict[str, Any]:
    """比较 `(transition_kind, page)` multiset。"""

    model_counts = delta_multiset_counts(model_rows)
    observed_counts = delta_multiset_counts(observed_rows)
    mismatches: list[dict[str, Any]] = []
    for key in sorted(set(model_counts) | set(observed_counts)):
        model_count = model_counts.get(key, 0)
        observed_count = observed_counts.get(key, 0)
        if model_count == observed_count:
            continue
        kind, page = key
        mismatches.append(
            {
                "transition_kind": kind,
                "page": page,
                "model_count": model_count,
                "observed_count": observed_count,
                "missing_in_model": max(observed_count - model_count, 0),
                "extra_in_model": max(model_count - observed_count, 0),
            }
        )
    return {
        "match": not mismatches,
        "mismatch_count": len(mismatches),
        "mismatch_totals_by_kind": summarize_delta_mismatches_by_kind(mismatches),
        "top_mismatches": mismatches[:sample_limit],
    }


def delta_multiset_counts(rows: list[dict[str, Any]]) -> dict[tuple[str, str], int]:
    """统计 delta row 中每个 kind/page 的出现次数。"""

    counts: dict[tuple[str, str], int] = {}
    for row in rows:
        kind = str(row.get("transition_kind") or "")
        if not kind:
            continue
        for page in row.get("pages", []):
            if page is None:
                continue
            key = (kind, str(page))
            counts[key] = counts.get(key, 0) + 1
    return counts


def count_rows_by_transition_kind(rows: list[dict[str, Any]]) -> dict[str, int]:
    """按 transition kind 汇总触达页数。"""

    counts: dict[str, int] = {}
    for row in rows:
        kind = str(row.get("transition_kind") or "")
        if not kind:
            continue
        counts[kind] = counts.get(kind, 0) + row_page_count(row)
    return dict(sorted(counts.items()))


def row_page_count(row: dict[str, Any]) -> int:
    """统计一条 delta row 中有效 page 数。"""

    pages = row.get("pages")
    if not isinstance(pages, list):
        return 0
    return sum(1 for page in pages if page is not None)


def summarize_delta_mismatches_by_kind(mismatches: list[dict[str, Any]]) -> dict[str, dict[str, int]]:
    """按 kind 汇总 multiset mismatch。"""

    summary: dict[str, dict[str, int]] = {}
    for row in mismatches:
        kind = str(row.get("transition_kind") or "")
        item = summary.setdefault(kind, {"mismatch_rows": 0, "missing_in_model": 0, "extra_in_model": 0})
        item["mismatch_rows"] += 1
        item["missing_in_model"] += int(row.get("missing_in_model") or 0)
        item["extra_in_model"] += int(row.get("extra_in_model") or 0)
    return dict(sorted(summary.items()))


def classify_transition_comparison_failure(
    self_check: dict[str, Any],
    observed: dict[str, Any],
    final_state: dict[str, Any],
    count_match: dict[str, Any],
    lifecycle_match: dict[str, Any],
) -> str:
    """把 transition exactness failure 粗分流。"""

    if not self_check.get("ready"):
        return "model_trace_incomplete"
    if not observed.get("oracle_ready"):
        return "observed_oracle_incomplete"
    if not final_state.get("match"):
        return "real_semantic_mismatch_or_final_state_regression"
    if not count_match.get("match") or not lifecycle_match.get("match"):
        return "transition_semantic_or_snapshot_observability_mismatch"
    return "matched"


def comparison_context_from_prediction(prediction_paths: PathsForPrediction, observed_path: Path, comparison: dict[str, Any]) -> dict[str, Any]:
    """从单格 prediction 路径构造比较上下文。"""

    return {
        "label": prediction_paths.prediction_dir.name,
        "input_id": comparison.get("input_id", ""),
        "source_config_id": "",
        "target_config_id": comparison.get("target_config_id", ""),
        "source_run_id": "",
        "target_run_id": comparison.get("target_run_id", ""),
        "is_self": comparison.get("comparison_mode") == "self",
        "prediction_dir": str(prediction_paths.prediction_dir),
        "observed_target_trace_path": str(observed_path),
        "transition_exactness_path": "",
    }


def comparison_context_from_matrix_row(matrix_row: dict[str, Any], prediction_dir: Path, observed_path: Path, comparison_path: Path) -> dict[str, Any]:
    """从矩阵行构造比较上下文。"""

    return {
        "label": matrix_row.get("label"),
        "input_id": matrix_row.get("input_id"),
        "source_config_id": matrix_row.get("source_config_id"),
        "target_config_id": matrix_row.get("target_config_id"),
        "source_run_id": matrix_row.get("source_run_id"),
        "target_run_id": matrix_row.get("target_run_id"),
        "is_self": matrix_row.get("is_self"),
        "prediction_dir": str(prediction_dir),
        "observed_target_trace_path": str(observed_path),
        "transition_exactness_path": str(comparison_path),
    }


# transition taxonomy / catalog / gate 的派生产物生成逻辑放在独立模块中。
# 本文件只保留 CLI、oracle 抽取、compare 和 matrix 编排。

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


def compare_transition_matrix(
    matrix_dir: Path,
    *,
    page_key_mode: str,
    force: bool,
    sample_limit: int,
    emit_catalog: bool,
    emit_gates: bool,
    catalog_output: Path | None,
    gate_output: Path | None,
    matrix_output_path: Path | None,
    progress: bool = False,
) -> dict[str, Any]:
    """构建矩阵级 transition exactness 汇总。"""

    plan_path = matrix_dir / "matrix_plan.json"
    if not plan_path.is_file():
        raise FileNotFoundError(f"missing matrix plan: {plan_path}")
    plan = load_json(plan_path)
    target_runs = target_runs_from_matrix_plan(plan)
    rebuilt_oracle_keys: set[tuple[str, str]] = set()
    rows = []
    classification_entries: list[dict[str, Any]] = []
    matrix_row_paths = sorted((matrix_dir / "predictions").glob("*/*/matrix_row.json"))
    total = len(matrix_row_paths)
    if progress:
        print_transition_stage_start(
            total,
            force=force,
            emit_catalog=emit_catalog,
            emit_gates=emit_gates,
        )
    for index, matrix_row_path in enumerate(matrix_row_paths, start=1):
        matrix_row = load_json(matrix_row_path)
        prediction_dir = resolve_repo_path(Path(str(matrix_row.get("output_dir") or matrix_row_path.parent)))
        prediction_paths = PathsForPrediction(
            prediction_dir=prediction_dir,
            predicted_trace=prediction_dir / "predicted_target_cache_state_trace.json",
            validation=prediction_dir / "validation.json",
            model_self_check=prediction_dir / "model_transition_self_check.json",
        )
        target_key = (str(matrix_row.get("input_id") or ""), str(matrix_row.get("target_config_id") or ""))
        target_run = target_runs.get(target_key)
        observed_path = observed_transition_path(matrix_dir, target_key)
        should_rebuild_oracle = target_run is not None and (
            (force and target_key not in rebuilt_oracle_keys) or not observed_path.is_file()
        )
        comparison_path = prediction_dir / (
            "transition_exactness_self.json"
            if matrix_row.get("is_self")
            else "transition_exactness_cross.json"
        )
        should_rebuild_comparison = (
            force
            or not comparison_path.is_file()
            or transition_comparison_needs_rebuild(comparison_path)
            or (emit_catalog and transition_comparison_lacks_catalog_evidence(comparison_path))
        )
        comparison_can_run = should_rebuild_comparison and prediction_paths.predicted_trace.is_file() and (
            observed_path.is_file() or target_run is not None
        )
        transition_should_run = should_rebuild_oracle or comparison_can_run
        if progress:
            print_transition_progress(
                index,
                total,
                matrix_row,
                matrix_row_path,
                should_run=transition_should_run,
                observed_path=observed_path,
                predicted_trace=prediction_paths.predicted_trace,
                target_run=target_run,
                should_rebuild_comparison=should_rebuild_comparison,
            )
        if should_rebuild_oracle:
            oracle = extract_target_oracle(
                [Path(path) for path in target_run["python_probe_files"]],
                target_run,
                sample_limit=sample_limit,
            )
            write_json(observed_path, oracle)
            rebuilt_oracle_keys.add(target_key)
        if observed_path.is_file() and prediction_paths.predicted_trace.is_file() and should_rebuild_comparison:
            comparison = compare_prediction_to_observed(
                prediction_paths,
                observed_path,
                comparison_mode="self" if matrix_row.get("is_self") else "cross",
                page_key_mode=page_key_mode,
                sample_limit=sample_limit,
                force_self_check=force,
                context=comparison_context_from_matrix_row(matrix_row, prediction_dir, observed_path, comparison_path),
                include_classification_evidence=emit_catalog,
            )
            write_json(comparison_path, comparison)
        row = summarize_matrix_transition_row(matrix_row, comparison_path, observed_path)
        if isinstance(row.get("transition_classification"), dict):
            classification_entries.append(row["transition_classification"])
        rows.append(row)
        if progress and transition_should_run:
            print_transition_result(index, total, row)

    summary = {
        "schema": "trace_sim.hicache.transition_exactness_matrix.v1",
        "matrix_dir": str(matrix_dir),
        "prediction_count": len(rows),
        "ready_count": sum(1 for row in rows if row.get("ready")),
        "exact_count": sum(1 for row in rows if row.get("exact")),
        "final_state_exact_count": sum(1 for row in rows if row.get("final_state_exact")),
        "transition_count_exact_count": sum(1 for row in rows if row.get("transition_count_exact")),
        "page_lifecycle_multiset_exact_count": sum(1 for row in rows if row.get("page_lifecycle_multiset_exact")),
        "by_input": summarize_rows_by_key(rows, "input_id"),
        "by_target_config": summarize_rows_by_key(rows, "target_config_id"),
        "failure_classification_counts": count_rows_by_value(rows, "failure_classification"),
        "family_counts": count_rows_by_value(rows, "transition_family"),
        "classification_counts": count_rows_by_value(rows, "classification"),
        "patch_risk_counts": count_patch_risks(rows),
        "predictions": rows,
        "notes": [
            "Only predictions with final-state exactness should be treated as transition-comparable.",
            "The matrix summary reuses target-side observed oracle per input/config.",
            "Each prediction row includes transition family classification and patch gate fields.",
        ],
    }
    if emit_catalog:
        catalog_path = resolve_output(catalog_output, matrix_dir / "transition_mismatch_catalog.json")
        catalog = build_transition_mismatch_catalog_from_entries(
            matrix_dir,
            classification_entries,
            source_matrix_path=str(matrix_output_path or matrix_dir / "transition_exactness_matrix.json"),
            sample_limit=sample_limit,
        )
        write_transition_catalog_outputs(matrix_dir, catalog_path, catalog, sample_limit=sample_limit)
        summary["catalog_path"] = str(catalog_path)
    if emit_gates:
        gate_path = resolve_output(gate_output, matrix_dir / "transition_patch_gate_scoreboard.json")
        scoreboard = build_transition_patch_gate_scoreboard_from_entries(
            matrix_dir,
            classification_entries,
            page_key_mode=page_key_mode,
            sample_limit=sample_limit,
        )
        write_json(gate_path, scoreboard)
        summary["gate_scoreboard_path"] = str(gate_path)
    return summary


def print_transition_stage_start(
    prediction_count: int,
    *,
    force: bool,
    emit_catalog: bool,
    emit_gates: bool,
) -> None:
    """打印 transition 阶段开始行。"""

    extras = []
    if force:
        extras.append("force=true")
    if emit_catalog:
        extras.append("catalog=true")
    if emit_gates:
        extras.append("gates=true")
    suffix = f" {' '.join(extras)}" if extras else ""
    print(
        f"[running transition] transition exactness matrix: predictions={prediction_count}{suffix}",
        flush=True,
    )


def print_transition_progress(
    index: int,
    total: int,
    matrix_row: dict[str, Any],
    matrix_row_path: Path,
    *,
    should_run: bool,
    observed_path: Path,
    predicted_trace: Path,
    target_run: dict[str, Any] | None,
    should_rebuild_comparison: bool,
) -> None:
    """打印 transition matrix 进度行。"""

    label = transition_progress_label(matrix_row, matrix_row_path)
    if should_run:
        print(f"[{index}/{total}] run {label}", flush=True)
        return
    reason = transition_skip_reason(
        observed_path,
        predicted_trace,
        target_run=target_run,
        should_rebuild_comparison=should_rebuild_comparison,
    )
    suffix = f": {reason}" if reason else ""
    print(f"[{index}/{total}] skip {label}{suffix}", flush=True)


def print_transition_result(index: int, total: int, row: dict[str, Any]) -> None:
    """打印 transition exactness 的简短结果行。"""

    ready = row.get("ready")
    exact = row.get("exact")
    final_state_exact = row.get("final_state_exact")
    if ready is not True:
        status = "not_ready"
    elif exact is True:
        status = "ok"
    elif final_state_exact is False:
        status = "final_state_mismatch"
    else:
        status = "mismatch"
    print(
        f"[{index}/{total}] result {status} "
        f"ready={progress_value(ready)} exact={progress_value(exact)} "
        f"final_state_exact={progress_value(final_state_exact)} "
        f"transition_count_exact={progress_value(row.get('transition_count_exact'))}",
        flush=True,
    )


def progress_value(value: Any) -> str:
    """把进度行中的 Python 值转成短字符串。"""

    if isinstance(value, bool) or value is None:
        return json.dumps(value)
    return str(value)


def transition_progress_label(matrix_row: dict[str, Any], matrix_row_path: Path) -> str:
    """返回 transition 进度行中的矩阵标签。"""

    label = matrix_row.get("label")
    if label:
        return str(label)
    input_id = str(matrix_row.get("input_id") or matrix_row_path.parent.parent.name)
    source_config_id = str(matrix_row.get("source_config_id") or "")
    target_config_id = str(matrix_row.get("target_config_id") or "")
    if source_config_id or target_config_id:
        return f"{input_id}/{source_config_id}->{target_config_id}"
    return str(matrix_row_path.parent)


def transition_skip_reason(
    observed_path: Path,
    predicted_trace: Path,
    *,
    target_run: dict[str, Any] | None,
    should_rebuild_comparison: bool,
) -> str:
    """返回 transition 进度 skip 的简短原因。"""

    if not predicted_trace.is_file():
        return "missing_predicted_trace"
    if not observed_path.is_file() and target_run is None:
        return "missing_observed_target_trace"
    if not should_rebuild_comparison:
        return ""
    return "not_ready"


def observed_transition_path(matrix_dir: Path, target_key: tuple[str, str]) -> Path:
    """返回某个 input/config 的 target-side transition oracle 路径。"""

    input_id, target_config_id = target_key
    return (
        matrix_dir
        / "observed_target_transitions"
        / safe_slug(input_id)
        / f"{safe_slug(target_config_id)}.observed_target_transition_trace.json"
    )


def count_rows_by_value(rows: list[dict[str, Any]], field: str) -> dict[str, int]:
    """按矩阵行字段值计数。"""

    return dict(
        sorted(
            collections.Counter(str(row.get(field) or "") for row in rows).items()
        )
    )


def count_patch_risks(rows: list[dict[str, Any]]) -> dict[str, int]:
    """按 patch gate 风险字段计数。"""

    return dict(
        sorted(
            collections.Counter(
                str(row.get("patch_gate", {}).get("patch_risk") or "")
                for row in rows
                if isinstance(row.get("patch_gate"), dict)
            ).items()
        )
    )


def transition_comparison_needs_rebuild(path: Path) -> bool:
    """判断 per-prediction transition exactness 是否仍是旧字段 schema。"""

    if not path.is_file():
        return True
    try:
        payload = load_json(path)
    except (OSError, json.JSONDecodeError):
        return True
    if not isinstance(payload, dict):
        return True
    required = (
        "final_state_exact",
        "transition_count_exact",
        "page_lifecycle_multiset_exact",
        "transition_classification",
        "patch_gate",
    )
    return any(key not in payload for key in required)


def transition_comparison_lacks_catalog_evidence(path: Path) -> bool:
    """判断 compare 输出是否缺少 catalog 需要的证据摘要。"""

    if not path.is_file():
        return True
    try:
        payload = load_json(path)
    except (OSError, json.JSONDecodeError):
        return True
    classification = payload.get("transition_classification") if isinstance(payload, dict) else None
    if not isinstance(classification, dict):
        return True
    return "hicache_evidence" not in classification or "observed_evidence" not in classification


def target_runs_from_matrix_plan(plan: dict[str, Any]) -> dict[tuple[str, str], dict[str, Any]]:
    """从 matrix_plan.json 构造 target run 索引。"""

    result: dict[tuple[str, str], dict[str, Any]] = {}
    for row in plan.get("runs", []):
        if not isinstance(row, dict):
            continue
        manifest_path = map_repo_path(Path(str(row.get("manifest_path") or "")))
        if not manifest_path.is_file():
            continue
        run = profile_run_from_manifest(manifest_path)
        key = (run.input_id, run.config_id)
        result[key] = {
            "target_manifest_path": str(run.manifest_path),
            "target_run_dir": str(run.run_dir),
            "target_run_id": run.run_id,
            "target_config_id": run.config_id,
            "input_id": run.input_id,
            "input_class": run.input_class,
            "python_probe_files": [str(path) for path in run.python_probe_files],
        }
    return result


def summarize_matrix_transition_row(matrix_row: dict[str, Any], comparison_path: Path, observed_path: Path) -> dict[str, Any]:
    """从 per-prediction exactness 输出提取矩阵行。"""

    base = {
        "label": matrix_row.get("label"),
        "input_id": matrix_row.get("input_id"),
        "source_config_id": matrix_row.get("source_config_id"),
        "target_config_id": matrix_row.get("target_config_id"),
        "source_run_id": matrix_row.get("source_run_id"),
        "target_run_id": matrix_row.get("target_run_id"),
        "is_self": matrix_row.get("is_self"),
        "prediction_dir": matrix_row.get("output_dir"),
        "observed_target_trace_path": str(observed_path),
        "transition_exactness_path": str(comparison_path),
        "final_state_match": matrix_row.get("final_state_match"),
    }
    if not comparison_path.is_file():
        return {
            **base,
            "ready": False,
            "exact": False,
            "failure_classification": "missing_transition_exactness_output",
            "final_state_exact": False,
            "transition_count_exact": False,
            "page_lifecycle_multiset_exact": False,
            "transition_family": "model_or_oracle_not_ready",
            "classification": "observed_unobservable",
            "patch_gate": {
                "patch_allowed": False,
                "patch_filter_action": "blocked",
                "patch_risk": "blocked",
                "source_attribution_required": False,
                "duration_required": False,
                "evidence_required": ["transition exactness output"],
            },
        }
    comparison = load_json(comparison_path)
    classification_entry = comparison.get("transition_classification") if isinstance(comparison.get("transition_classification"), dict) else {}
    return {
        **base,
        "ready": comparison.get("ready"),
        "exact": comparison.get("exact"),
        "model_transition_self_check_ready": comparison.get("model_transition_self_check_ready"),
        "oracle_ready": comparison.get("oracle_ready"),
        "final_state_exact": comparison.get("final_state_exact"),
        "transition_count_exact": comparison.get("transition_count_exact"),
        "page_lifecycle_multiset_exact": comparison.get("page_lifecycle_multiset_exact"),
        "failure_classification": comparison.get("failure_classification"),
        "transition_family": comparison.get("transition_family"),
        "classification": comparison.get("classification"),
        "classification_reason": comparison.get("classification_reason"),
        "mismatch_kinds": comparison.get("mismatch_kinds", []),
        "patch_gate": comparison.get("patch_gate", {}),
        "transition_classification": classification_entry,
        "model_delta_count_by_kind": comparison.get("model_delta_count_by_kind", {}),
        "observed_delta_count_by_kind": comparison.get("observed_delta_count_by_kind", {}),
        "mismatch_totals_by_kind": comparison.get("page_lifecycle_multiset_comparison", {}).get("mismatch_totals_by_kind", {}),
    }


def summarize_rows_by_key(rows: list[dict[str, Any]], key: str) -> dict[str, Any]:
    """按矩阵字段汇总通过数。"""

    result: dict[str, Any] = {}
    for value in sorted({str(row.get(key) or "") for row in rows}):
        selected = [row for row in rows if str(row.get(key) or "") == value]
        result[value] = {
            "prediction_count": len(selected),
            "ready_count": sum(1 for row in selected if row.get("ready")),
            "exact_count": sum(1 for row in selected if row.get("exact")),
            "final_state_exact_count": sum(1 for row in selected if row.get("final_state_exact")),
            "transition_count_exact_count": sum(1 for row in selected if row.get("transition_count_exact")),
            "page_lifecycle_multiset_exact_count": sum(1 for row in selected if row.get("page_lifecycle_multiset_exact")),
        }
    return result


def optional_int(value: Any, default: int = 0) -> int:
    """宽松解析整数。"""

    try:
        return int(float(value))
    except (TypeError, ValueError):
        return default


if __name__ == "__main__":
    raise SystemExit(main())
