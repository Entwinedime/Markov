"""单个 prediction 的 HiCache transition exactness 比较。"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from markov_internal.common.io import load_json, write_json
from ...oracle.diff.multiset import count_rows_by_transition_kind
from ..artifacts.paths import PathsForPrediction
from ..replay.engine import replay_predicted_records
from ..replay.record_schema import (
    load_predicted_trace,
    predicted_final_state,
    predicted_records,
)
from .delta_rows import (
    comparable_model_delta_rows,
    comparable_observed_delta_rows,
    compare_page_lifecycle_multiset,
    compare_transition_kind_counts,
    final_state_comparison,
)
from .self_check import build_model_self_check
from .taxonomy import (
    build_transition_classification_entry,
    compare_result_classification_fields,
)
from .taxonomy_evidence import load_hicache_summary


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
    lifecycle_match = compare_page_lifecycle_multiset(model_rows, observed_rows, sample_limit=sample_limit)
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


def comparison_context_from_prediction_row(
    prediction_row: dict[str, Any],
    prediction_dir: Path,
    observed_path: Path,
    comparison_path: Path,
) -> dict[str, Any]:
    """从 prediction row 构造比较上下文。"""

    return {
        "label": prediction_row.get("label"),
        "input_id": prediction_row.get("input_id"),
        "source_config_id": prediction_row.get("source_config_id"),
        "target_config_id": prediction_row.get("target_config_id"),
        "source_run_id": prediction_row.get("source_run_id"),
        "target_run_id": prediction_row.get("target_run_id"),
        "is_self": prediction_row.get("is_self"),
        "prediction_dir": str(prediction_dir),
        "observed_target_trace_path": str(observed_path),
        "transition_exactness_path": str(comparison_path),
    }
