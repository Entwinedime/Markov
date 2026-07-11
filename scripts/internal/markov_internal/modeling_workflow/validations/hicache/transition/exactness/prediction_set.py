"""Prediction-set orchestration for HiCache transition exactness."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any
from collections.abc import Callable

from markov_internal.common.io import load_json, output_is_current, write_json

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
    catalog_output: Path | None,
    gate_output: Path | None,
    summary_output_path: Path | None,
    on_row: Callable[[dict[str, Any]], None] | None = None,
) -> dict[str, Any]:
    """Validate a complete set of transition prediction rows."""

    options = TransitionPredictionSetOptions(
        artifact_root=artifact_root,
        page_key_mode=page_key_mode,
        force=force,
        sample_limit=sample_limit,
        catalog_output=catalog_output,
        gate_output=gate_output,
        summary_output_path=summary_output_path,
        on_row=on_row,
    )
    return TransitionPredictionSetValidator(prediction_rows, target_runs, options).run()


@dataclass(frozen=True)
class TransitionPredictionSetOptions:
    """Options governing oracle reuse, comparison, and diagnostics."""

    artifact_root: Path
    page_key_mode: str
    force: bool
    sample_limit: int
    catalog_output: Path | None
    gate_output: Path | None
    summary_output_path: Path | None
    on_row: Callable[[dict[str, Any]], None] | None = None


@dataclass
class TransitionPredictionSetValidator:
    """Manage target-oracle reuse, per-cell comparison, and summaries."""

    prediction_rows: list[dict[str, Any]]
    target_runs: dict[tuple[str, str], dict[str, Any]]
    options: TransitionPredictionSetOptions
    rebuilt_oracle_keys: set[tuple[str, str]] = field(default_factory=set)
    rows: list[dict[str, Any]] = field(default_factory=list)
    classification_entries: list[dict[str, Any]] = field(default_factory=list)

    def run(self) -> dict[str, Any]:
        """Execute the complete prediction-set validation."""

        for prediction_row in self.prediction_rows:
            self.process_prediction(prediction_row)
        summary = self.build_summary()
        self.write_diagnostic_artifacts(summary)
        return summary

    def process_prediction(self, prediction_row: dict[str, Any]) -> None:
        """Process one source-to-target prediction cell."""

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
        """Build the target-side oracle when its reuse contract is stale."""

        target_run = self.target_runs.get(target_key)
        should_rebuild = target_run is not None and (
            (self.options.force and target_key not in self.rebuilt_oracle_keys)
            or not target_oracle_is_current(observed_path, target_run, sample_limit=self.options.sample_limit)
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
        """Build one exactness payload when its inputs or options changed."""

        comparison_mode = "self" if prediction_row.get("is_self") else "cross"
        input_paths = [
            prediction_paths.predicted_trace,
            prediction_paths.validation,
            observed_path,
        ]
        model_summary_path = prediction_dir / "model_summary.json"
        if model_summary_path.is_file():
            input_paths.append(model_summary_path)
        should_rebuild = self.options.force or not comparison_is_current(
            comparison_path,
            input_paths,
            comparison_mode=comparison_mode,
            page_key_mode=self.options.page_key_mode,
            sample_limit=self.options.sample_limit,
        )
        if not observed_path.is_file() or not prediction_paths.predicted_trace.is_file() or not should_rebuild:
            return
        comparison = compare_prediction_to_observed(
            prediction_paths,
            observed_path,
            comparison_mode=comparison_mode,
            page_key_mode=self.options.page_key_mode,
            sample_limit=self.options.sample_limit,
            force_self_check=self.options.force,
            context=comparison_context_from_prediction_row(
                prediction_row, prediction_dir, observed_path, comparison_path
            ),
            include_classification_evidence=True,
        )
        write_json(comparison_path, comparison)

    def build_summary(self) -> dict[str, Any]:
        """Aggregate exactness results across all prediction cells."""

        rows = self.rows
        return {
            "schema": "trace_sim.hicache.transition_exactness_summary.v1",
            "artifact_root": str(self.options.artifact_root),
            "prediction_count": len(rows),
            "ready_count": sum(1 for row in rows if row.get("ready")),
            "exact_count": sum(1 for row in rows if row.get("exact")),
            "skipped_count": sum(1 for row in rows if row.get("skipped")),
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

    def write_diagnostic_artifacts(self, summary: dict[str, Any]) -> None:
        """Write the mandatory HiCache catalog and gate diagnostics."""

        self.write_catalog(summary)
        self.write_gate_scoreboard(summary)

    def write_catalog(self, summary: dict[str, Any]) -> None:
        """Write the mandatory transition mismatch catalog."""

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
            catalog_path,
            catalog,
            sample_limit=self.options.sample_limit,
        )
        summary["catalog_path"] = str(catalog_path)

    def write_gate_scoreboard(self, summary: dict[str, Any]) -> None:
        """Write the mandatory diagnostic patch-gate scoreboard."""

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


def target_oracle_is_current(
    observed_path: Path,
    target_run: dict[str, Any],
    *,
    sample_limit: int,
) -> bool:
    """Validate target-oracle metadata and modification-time dependencies."""

    trace_paths = [Path(path) for path in target_run.get("python_probe_files", [])]
    if not output_is_current(observed_path, trace_paths):
        return False
    try:
        payload = load_json(observed_path)
    except (OSError, ValueError):
        return False
    if not isinstance(payload, dict):
        return False
    metadata_fields = ("target_run_id", "target_config_id", "input_id", "input_class")
    return (
        payload.get("sample_limit") == sample_limit
        and payload.get("oracle_trace_files") == [str(path) for path in trace_paths]
        and all(payload.get(field) == target_run.get(field) for field in metadata_fields)
    )


def comparison_is_current(
    comparison_path: Path,
    input_paths: list[Path],
    *,
    comparison_mode: str,
    page_key_mode: str,
    sample_limit: int,
) -> bool:
    """Validate comparison options and ensure no input artifact is newer."""

    if not output_is_current(comparison_path, input_paths):
        return False
    try:
        payload = load_json(comparison_path)
    except (OSError, ValueError):
        return False
    return bool(
        isinstance(payload, dict)
        and payload.get("comparison_mode") == comparison_mode
        and payload.get("page_key_mode") == page_key_mode
        and payload.get("sample_limit") == sample_limit
    )
