"""Workflow validation object for HiCache transition exactness."""

from __future__ import annotations

from typing import Any

from .....common.io import load_json, write_json
from ....types import ModelRunResult, ModelRunSpec, ModelOutputRequirement, ValidationSummary
from ...registry import PredictionValidation
from ..prediction_rows import build_prediction_row
from ..preflight.state_input_preflight import HiCacheStateInputPreflightCheck
from .closure import build_transition_closure_report
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
            final_dag_rows = self._final_dag_rows(context, rows)
            final_state_closure_rows = self._final_state_closure_rows(context)
            closure = build_transition_closure_report(
                rows,
                final_dag_rows,
                final_state_closure_rows,
            )
            closure_path = artifact_root / "transition_closure_report.json"
            write_json(closure_path, closure)
            for row in rows:
                model_run_id = str(row.get("model_run_id") or "")
                if model_run_id:
                    write_json(context.artifacts.validation_row_path(self.name, model_run_id), row)
            summary = {
                **summary,
                "schema": "trace_sim.modeling_workflow.validation.hicache_transition.v1",
                "validation": self.name,
                "status": self._status(summary),
                "artifact_root": str(artifact_root),
                "closure_report_path": str(closure_path),
                "closure_review_ready": closure["review_ready"],
                "closure_classification_counts": closure["classification_counts"],
                "closure_unrelated_semantic_mismatch_count": closure["unrelated_semantic_mismatch_count"],
                "closure_not_ready_count": closure["not_ready_count"],
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
            artifact_paths={
                "summary": str(context.artifacts.validation_summary_path(self.name)),
                **({"closure": str(summary["closure_report_path"])} if summary.get("closure_report_path") else {}),
            },
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
    def _final_dag_rows(context: Any, rows: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
        """Load final-DAG evidence produced earlier in the same workflow."""

        result: dict[str, dict[str, Any]] = {}
        for row in rows:
            model_run_id = str(row.get("model_run_id") or "")
            if not model_run_id:
                continue
            path = context.artifacts.validation_row_path("final_dag", model_run_id)
            if not path.is_file():
                continue
            payload = load_json(path)
            if isinstance(payload, dict):
                result[model_run_id] = payload
        return result

    @staticmethod
    def _final_state_closure_rows(context: Any) -> dict[str, dict[str, Any]]:
        """Load final-state closure evidence produced earlier in the workflow."""

        path = context.artifacts.validations_dir / "hicache_final_state" / "final_state_closure_report.json"
        if not path.is_file():
            return {}
        payload = load_json(path)
        rows = payload.get("rows") if isinstance(payload, dict) else None
        if not isinstance(rows, list):
            return {}
        return {str(row.get("model_run_id")): row for row in rows if isinstance(row, dict) and row.get("model_run_id")}

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
        closure_counts = summary.get("closure_classification_counts") or {}
        return (
            f"{prediction_count} predictions | "
            f"raw exact {summary.get('exact_count')}/{prediction_count} | "
            f"readiness-limit "
            f"{closure_counts.get('payload_only_prefetch_readiness_limitation', 0)} | "
            f"snapshot {closure_counts.get('snapshot_grouping_or_observability', 0)} | "
            f"unresolved {summary.get('closure_unrelated_semantic_mismatch_count', 0)}"
        )
