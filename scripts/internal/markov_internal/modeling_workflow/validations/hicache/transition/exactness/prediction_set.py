"""基于 workflow prediction rows 的 HiCache transition exactness 验证。"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

from markov_internal.common.io import write_json

from ..artifacts.catalog import (
    build_transition_mismatch_catalog_from_entries,
    write_transition_catalog_outputs,
)
from ..artifacts.paths import PathsForPrediction, resolve_output
from .compare import (
    compare_prediction_to_observed,
    comparison_context_from_prediction_row,
)
from .gate import build_transition_patch_gate_scoreboard_from_entries
from .oracle import extract_target_oracle
from .prediction_summary import (
    comparison_path_for_prediction,
    count_patch_risks,
    count_rows_by_value,
    observed_transition_path,
    prediction_dir_from_row,
    prediction_paths_for_dir,
    summarize_rows_by_key,
    summarize_transition_prediction_row,
    target_key_from_row,
)


def compare_transition_prediction_rows(
    prediction_rows: list[dict[str, Any]],
    target_runs: dict[tuple[str, str], dict[str, Any]],
    *,
    artifact_root: Path,
    page_key_mode: str,
    force: bool,
    sample_limit: int,
    emit_catalog: bool,
    emit_gates: bool,
    catalog_output: Path | None,
    gate_output: Path | None,
    summary_output_path: Path | None,
    on_row: Callable[[dict[str, Any]], None] | None = None,
) -> dict[str, Any]:
    """验证一组 transition prediction rows。"""

    options = TransitionPredictionSetOptions(
        artifact_root=artifact_root,
        page_key_mode=page_key_mode,
        force=force,
        sample_limit=sample_limit,
        emit_catalog=emit_catalog,
        emit_gates=emit_gates,
        catalog_output=catalog_output,
        gate_output=gate_output,
        summary_output_path=summary_output_path,
        on_row=on_row,
    )
    return TransitionPredictionSetValidator(prediction_rows, target_runs, options).run()


@dataclass(frozen=True)
class TransitionPredictionSetOptions:
    """transition prediction set 验证的运行选项。"""

    artifact_root: Path
    page_key_mode: str
    force: bool
    sample_limit: int
    emit_catalog: bool
    emit_gates: bool
    catalog_output: Path | None
    gate_output: Path | None
    summary_output_path: Path | None
    on_row: Callable[[dict[str, Any]], None] | None = None


@dataclass
class TransitionPredictionSetValidator:
    """管理 target oracle 复用、逐 prediction 比较和汇总产物写出。"""

    prediction_rows: list[dict[str, Any]]
    target_runs: dict[tuple[str, str], dict[str, Any]]
    options: TransitionPredictionSetOptions
    rebuilt_oracle_keys: set[tuple[str, str]] = field(default_factory=set)
    rows: list[dict[str, Any]] = field(default_factory=list)
    classification_entries: list[dict[str, Any]] = field(default_factory=list)

    def run(self) -> dict[str, Any]:
        """执行完整 prediction set 验证。"""

        for prediction_row in self.prediction_rows:
            self.process_prediction(prediction_row)
        summary = self.build_summary()
        self.write_optional_artifacts(summary)
        return summary

    def process_prediction(self, prediction_row: dict[str, Any]) -> None:
        """处理单个 source->target prediction。"""

        prediction_dir = prediction_dir_from_row(prediction_row, self.options.artifact_root)
        prediction_paths = prediction_paths_for_dir(prediction_dir)
        target_key = target_key_from_row(prediction_row)
        observed_path = observed_transition_path(self.options.artifact_root, target_key)
        comparison_path = comparison_path_for_prediction(prediction_dir, prediction_row)
        self.ensure_target_oracle(target_key, observed_path)
        self.ensure_comparison(prediction_row, prediction_dir, prediction_paths, observed_path, comparison_path)
        row = summarize_transition_prediction_row(prediction_row, comparison_path, observed_path)
        if isinstance(row.get("transition_classification"), dict):
            self.classification_entries.append(row["transition_classification"])
        self.rows.append(row)
        if self.options.on_row is not None:
            self.options.on_row(row)

    def ensure_target_oracle(self, target_key: tuple[str, str], observed_path: Path) -> None:
        """必要时构造 target-side observed transition oracle。"""

        target_run = self.target_runs.get(target_key)
        should_rebuild = target_run is not None and (
            (self.options.force and target_key not in self.rebuilt_oracle_keys) or not observed_path.is_file()
        )
        if not should_rebuild:
            return
        oracle = extract_target_oracle(
            [Path(path) for path in target_run["python_probe_files"]],
            target_run,
            sample_limit=self.options.sample_limit,
        )
        write_json(observed_path, oracle)
        self.rebuilt_oracle_keys.add(target_key)

    def ensure_comparison(
        self,
        prediction_row: dict[str, Any],
        prediction_dir: Path,
        prediction_paths: PathsForPrediction,
        observed_path: Path,
        comparison_path: Path,
    ) -> None:
        """必要时构造单个 prediction 的 exactness payload。"""

        should_rebuild = self.options.force or self.options.emit_catalog or not comparison_path.is_file()
        if not observed_path.is_file() or not prediction_paths.predicted_trace.is_file() or not should_rebuild:
            return
        comparison = compare_prediction_to_observed(
            prediction_paths,
            observed_path,
            comparison_mode="self" if prediction_row.get("is_self") else "cross",
            page_key_mode=self.options.page_key_mode,
            sample_limit=self.options.sample_limit,
            force_self_check=self.options.force,
            context=comparison_context_from_prediction_row(
                prediction_row, prediction_dir, observed_path, comparison_path
            ),
            include_classification_evidence=self.options.emit_catalog,
        )
        write_json(comparison_path, comparison)

    def build_summary(self) -> dict[str, Any]:
        """汇总整组 prediction 的 transition exactness 结果。"""

        rows = self.rows
        return {
            "schema": "trace_sim.hicache.transition_exactness_summary.v1",
            "artifact_root": str(self.options.artifact_root),
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
            "notes": [
                "Only predictions with final-state exactness should be treated as transition-comparable.",
                "The prediction-set summary reuses target-side observed oracle per input/config.",
                "Full per-prediction exactness payloads live beside each prediction row.",
            ],
        }

    def write_optional_artifacts(self, summary: dict[str, Any]) -> None:
        """按 CLI 开关写出 catalog 和 gate 诊断产物。"""

        if self.options.emit_catalog:
            self.write_catalog(summary)
        if self.options.emit_gates:
            self.write_gate_scoreboard(summary)

    def write_catalog(self, summary: dict[str, Any]) -> None:
        """写出 transition mismatch catalog。"""

        catalog_path = resolve_output(
            self.options.catalog_output,
            self.options.artifact_root / "transition_mismatch_catalog.json",
        )
        catalog = build_transition_mismatch_catalog_from_entries(
            self.options.artifact_root,
            self.classification_entries,
            source_summary_path=str(
                self.options.summary_output_path or self.options.artifact_root / "transition_exactness_summary.json"
            ),
            sample_limit=self.options.sample_limit,
        )
        write_transition_catalog_outputs(
            self.options.artifact_root,
            catalog_path,
            catalog,
            sample_limit=self.options.sample_limit,
        )
        summary["catalog_path"] = str(catalog_path)

    def write_gate_scoreboard(self, summary: dict[str, Any]) -> None:
        """写出 transition patch gate scoreboard。"""

        gate_path = resolve_output(
            self.options.gate_output,
            self.options.artifact_root / "transition_patch_gate_scoreboard.json",
        )
        scoreboard = build_transition_patch_gate_scoreboard_from_entries(
            self.options.artifact_root,
            self.classification_entries,
            page_key_mode=self.options.page_key_mode,
            sample_limit=self.options.sample_limit,
        )
        write_json(gate_path, scoreboard)
        summary["gate_scoreboard_path"] = str(gate_path)
