"""Workflow validation object for HiCache transition exactness."""

from __future__ import annotations

from typing import Any

from .....common.io import write_json
from ....types import ModelRunResult, ModelRunSpec, ModelOutputRequirement, ValidationSummary
from ...registry import PredictionValidation
from ..prediction_rows import build_prediction_row
from ..preflight.state_input_preflight import HiCacheStateInputPreflightCheck
from .exactness.prediction_set import compare_transition_prediction_rows


class HiCacheTransitionValidation(PredictionValidation):
    """Validate transition exactness using cache-state model artifacts."""

    name = "hicache_transition"
    cache_state_output_requirements = frozenset({ModelOutputRequirement.HICACHE_VALIDATION})

    def preflight_checks(self) -> tuple[type[HiCacheStateInputPreflightCheck], ...]:
        """Require the shared HiCache state and oracle-input preflight."""

        return (HiCacheStateInputPreflightCheck,)

    def analyze(
        self,
        context: Any,
        specs: list[ModelRunSpec],
        results: dict[str, ModelRunResult],
    ) -> ValidationSummary:
        """Compare cache-state prediction rows with target transition oracles."""

        selected = [spec for spec in specs if self.name in spec.validation_requests]
        prediction_rows = [build_prediction_row(results[spec.run_id]) for spec in selected]
        model_run_ids_by_label = {str(row.get("label") or ""): row.get("model_run_id") for row in prediction_rows}
        target_runs = self._target_runs(context)
        artifact_root = context.artifacts.validations_dir / self.name
        rows: list[dict[str, Any]] = []
        progress = context.reporter.start_stage(
            self.name, len(prediction_rows), "HiCache transition", unit="prediction"
        )

        def on_row(row: dict[str, Any]) -> None:
            rows.append(row)
            model_run_id = model_run_ids_by_label.get(str(row.get("label") or ""))
            if model_run_id:
                write_json(context.artifacts.validation_row_path(self.name, str(model_run_id)), row)
            progress.advance(
                {
                    "ready": f"{sum(1 for item in rows if item.get('ready'))}/{len(rows)}",
                    "exact": f"{sum(1 for item in rows if item.get('exact'))}/{len(rows)}",
                }
            )

        if context.options.dry_run:
            summary = {
                "schema": "trace_sim.modeling_workflow.validation.hicache_transition.v1",
                "validation": self.name,
                "dry_run": True,
                "prediction_count": len(prediction_rows),
                "ready_count": 0,
                "exact_count": 0,
                "skipped_count": len(prediction_rows),
                "final_state_exact_count": 0,
                "transition_count_exact_count": 0,
                "status": "CHECK" if prediction_rows else "EMPTY",
                "artifact_root": str(artifact_root),
            }
        else:
            summary = compare_transition_prediction_rows(
                prediction_rows,
                target_runs,
                artifact_root=artifact_root,
                page_key_mode=context.options.page_key_mode,
                force=context.options.force,
                sample_limit=context.options.sample_limit,
                catalog_output=context.artifacts.validations_dir / self.name / "transition_mismatch_catalog.json",
                gate_output=context.artifacts.validations_dir / self.name / "transition_patch_gate_scoreboard.json",
                summary_output_path=context.artifacts.validation_summary_path(self.name),
                on_row=on_row,
            )
            summary = {
                **summary,
                "schema": "trace_sim.modeling_workflow.validation.hicache_transition.v1",
                "validation": self.name,
                "status": self._status(summary),
                "artifact_root": str(artifact_root),
            }
        write_json(context.artifacts.validation_summary_path(self.name), summary)
        progress.finish(summary["status"], self._summary_text(summary))
        return ValidationSummary(
            name=self.name,
            status=summary["status"],
            selected_run_count=int(summary.get("prediction_count") or 0),
            ready_count=int(summary.get("ready_count") or 0),
            exact_count=int(summary.get("exact_count") or 0),
            skipped_count=int(summary.get("skipped_count") or 0),
            artifact_paths={"summary": str(context.artifacts.validation_summary_path(self.name))},
            payload=summary,
        )

    @staticmethod
    def _target_runs(context: Any) -> dict[tuple[str, str], dict[str, Any]]:
        """Index target-run metadata required to build transition oracles."""

        return {
            (run.input_id, run.config_id): {
                "target_manifest_path": str(run.manifest_path),
                "target_run_dir": str(run.run_dir),
                "target_run_id": run.run_id,
                "target_config_id": run.config_id,
                "input_id": run.input_id,
                "input_class": run.input_class,
                "python_probe_files": [str(path) for path in run.python_probe_files],
            }
            for run in context.runs
        }

    @staticmethod
    def _status(summary: dict[str, Any]) -> str:
        prediction_count = int(summary.get("prediction_count") or 0)
        ready_count = int(summary.get("ready_count") or 0)
        exact_count = int(summary.get("exact_count") or 0)
        if prediction_count == 0:
            return "EMPTY"
        if ready_count != prediction_count:
            return "NOT_READY"
        if exact_count == prediction_count:
            return "OK"
        return "MISMATCH"

    @staticmethod
    def _summary_text(summary: dict[str, Any]) -> str:
        prediction_count = int(summary.get("prediction_count") or 0)
        return (
            f"{prediction_count} predictions | "
            f"exact {summary.get('exact_count')}/{prediction_count} | "
            f"ready {summary.get('ready_count')}/{prediction_count} | "
            f"count {summary.get('transition_count_exact_count')}/{prediction_count}"
        )
