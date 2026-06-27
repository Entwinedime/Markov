"""Single-prediction HiCache transition exactness comparison."""

from __future__ import annotations

import collections
from pathlib import Path
from typing import Any

from ..common.io import load_json, write_json
from .oracle_state import (
    normalize_hicache_page_key,
    normalize_hicache_state_for_oracle_compare,
)
from .transition_oracle import SNAPSHOT_VISIBLE_STATE_KEYS, TRANSITION_COMPARABLE_STATE_KEYS
from .transition_paths import PathsForPrediction
from .transition_record_schema import (
    STATE_DELTA_KINDS,
    count_rows_by_transition_kind,
    load_predicted_trace,
    predicted_final_state,
    predicted_records,
    state_counts,
)
from .transition_replay import replay_predicted_records
from .transition_self_check import build_model_self_check
from .transition_taxonomy import (
    build_transition_classification_entry,
    compare_result_classification_fields,
    load_hicache_summary,
)


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
    final_state = final_state_comparison(
        predicted_final_state(predicted),
        observed.get("final_state", {}),
        page_key_mode=page_key_mode,
        sample_limit=sample_limit,
    )
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
        "failure_classification": classify_transition_comparison_failure(
            self_check,
            observed,
            final_state,
            count_match,
            lifecycle_match,
        ),
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

    visible_delta_kinds = {
        kind for state_key in TRANSITION_COMPARABLE_STATE_KEYS for kind in STATE_DELTA_KINDS[state_key]
    }
    return normalize_delta_rows_to_union_timeline(
        [row for row in rows if str(row.get("transition_kind") or "") in visible_delta_kinds],
        page_key_mode,
    )


def comparable_observed_delta_rows(rows: Any, page_key_mode: str) -> list[dict[str, Any]]:
    """规整 observed snapshot delta rows。"""

    if not isinstance(rows, list):
        return []
    visible_delta_kinds = {
        kind for state_key in TRANSITION_COMPARABLE_STATE_KEYS for kind in STATE_DELTA_KINDS[state_key]
    }
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
                "pages": sorted(
                    {normalize_hicache_page_key(page, page_key_mode) for page in pages if page is not None}
                ),
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


def final_state_comparison(
    model_final: dict[str, Any],
    observed_final: dict[str, Any],
    *,
    page_key_mode: str,
    sample_limit: int,
) -> dict[str, Any]:
    """比较 model/oracle final state 的 snapshot 可见字段。"""

    normalized_model = normalize_hicache_state_for_oracle_compare(model_final, page_key_mode)
    normalized_observed = normalize_hicache_state_for_oracle_compare(observed_final, page_key_mode)
    diffs: dict[str, Any] = {}
    match = True
    for key in SNAPSHOT_VISIBLE_STATE_KEYS:
        model_pages = (
            {str(page) for page in normalized_model.get(key, []) if page is not None}
            if isinstance(normalized_model.get(key), list)
            else set()
        )
        observed_pages = (
            {str(page) for page in normalized_observed.get(key, []) if page is not None}
            if isinstance(normalized_observed.get(key), list)
            else set()
        )
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


def compare_delta_multisets(
    model_rows: list[dict[str, Any]],
    observed_rows: list[dict[str, Any]],
    *,
    sample_limit: int,
) -> dict[str, Any]:
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


def comparison_context_from_prediction(
    prediction_paths: PathsForPrediction,
    observed_path: Path,
    comparison: dict[str, Any],
) -> dict[str, Any]:
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


def comparison_context_from_matrix_row(
    matrix_row: dict[str, Any],
    prediction_dir: Path,
    observed_path: Path,
    comparison_path: Path,
) -> dict[str, Any]:
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
