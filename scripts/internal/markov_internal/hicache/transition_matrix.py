"""Matrix-level HiCache transition exactness orchestration."""

from __future__ import annotations

import collections
import json
from pathlib import Path
from typing import Any

from ..common.io import load_json, write_json
from .matrix_discovery import profile_run_from_manifest
from .matrix_types import safe_slug
from .transition_catalog import (
    build_transition_mismatch_catalog_from_entries,
    write_transition_catalog_outputs,
)
from .transition_compare import (
    compare_prediction_to_observed,
    comparison_context_from_matrix_row,
)
from .transition_gate import build_transition_patch_gate_scoreboard_from_entries
from .transition_oracle import extract_target_oracle
from .transition_paths import (
    PathsForPrediction,
    map_repo_path,
    resolve_output,
    resolve_repo_path,
)


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

    return dict(sorted(collections.Counter(str(row.get(field) or "") for row in rows).items()))


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
    classification_entry = (
        comparison.get("transition_classification")
        if isinstance(comparison.get("transition_classification"), dict)
        else {}
    )
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
        "mismatch_totals_by_kind": comparison.get("page_lifecycle_multiset_comparison", {}).get(
            "mismatch_totals_by_kind",
            {},
        ),
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
