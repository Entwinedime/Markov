"""transition prediction row 的路径与汇总工具。"""

from __future__ import annotations

import collections
from pathlib import Path
from typing import Any

from markov_internal.common.io import load_json
from markov_internal.common.naming import safe_slug

from ..artifacts.paths import PathsForPrediction, resolve_repo_path


def prediction_paths_for_dir(prediction_dir: Path) -> PathsForPrediction:
    """生成单个 prediction 的标准 transition artifact 路径。"""

    return PathsForPrediction(
        prediction_dir=prediction_dir,
        predicted_trace=prediction_dir / "predicted_target_cache_state_trace.json",
        validation=prediction_dir / "validation.json",
        model_self_check=prediction_dir / "model_transition_self_check.json",
    )


def target_key_from_row(prediction_row: dict[str, Any]) -> tuple[str, str]:
    """提取 transition oracle 复用使用的 target key。"""

    return str(prediction_row.get("input_id") or ""), str(prediction_row.get("target_config_id") or "")


def comparison_path_for_prediction(prediction_dir: Path, prediction_row: dict[str, Any]) -> Path:
    """返回单个 prediction 的 exactness payload 路径。"""

    filename = "transition_exactness_self.json" if prediction_row.get("is_self") else "transition_exactness_cross.json"
    return prediction_dir / filename


def prediction_dir_from_row(prediction_row: dict[str, Any], artifact_root: Path) -> Path:
    """返回 transition row 记录的 prediction 输出目录。"""

    raw_output_dir = prediction_row.get("output_dir")
    if raw_output_dir:
        return resolve_repo_path(Path(str(raw_output_dir)))
    input_id = str(prediction_row.get("input_id") or "")
    source_config_id = str(prediction_row.get("source_config_id") or "")
    target_config_id = str(prediction_row.get("target_config_id") or "")
    return (
        artifact_root
        / "predictions"
        / safe_slug(input_id)
        / f"{safe_slug(source_config_id)}__to__{safe_slug(target_config_id)}"
    )


def observed_transition_path(artifact_root: Path, target_key: tuple[str, str]) -> Path:
    """返回某个 input/config 的 target-side transition oracle 路径。"""

    input_id, target_config_id = target_key
    return (
        artifact_root
        / "observed_target_transitions"
        / safe_slug(input_id)
        / f"{safe_slug(target_config_id)}.observed_target_transition_trace.json"
    )


def count_rows_by_value(rows: list[dict[str, Any]], field: str) -> dict[str, int]:
    """按 prediction row 字段值计数。"""

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


def summarize_transition_prediction_row(
    prediction_row: dict[str, Any], comparison_path: Path, observed_path: Path
) -> dict[str, Any]:
    """从 per-prediction exactness 输出提取 prediction row。"""

    base = {
        "label": prediction_row.get("label"),
        "input_id": prediction_row.get("input_id"),
        "source_config_id": prediction_row.get("source_config_id"),
        "target_config_id": prediction_row.get("target_config_id"),
        "source_run_id": prediction_row.get("source_run_id"),
        "target_run_id": prediction_row.get("target_run_id"),
        "is_self": prediction_row.get("is_self"),
        "prediction_dir": prediction_row.get("output_dir"),
        "observed_target_trace_path": str(observed_path),
        "transition_exactness_path": str(comparison_path),
        "final_state_match": prediction_row.get("final_state_match"),
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
    """按 prediction 字段汇总通过数。"""

    result: dict[str, Any] = {}
    for value in sorted({str(row.get(key) or "") for row in rows}):
        selected = [row for row in rows if str(row.get(key) or "") == value]
        result[value] = {
            "prediction_count": len(selected),
            "ready_count": sum(1 for row in selected if row.get("ready")),
            "exact_count": sum(1 for row in selected if row.get("exact")),
            "final_state_exact_count": sum(1 for row in selected if row.get("final_state_exact")),
            "transition_count_exact_count": sum(1 for row in selected if row.get("transition_count_exact")),
            "page_lifecycle_multiset_exact_count": sum(
                1 for row in selected if row.get("page_lifecycle_multiset_exact")
            ),
        }
    return result
