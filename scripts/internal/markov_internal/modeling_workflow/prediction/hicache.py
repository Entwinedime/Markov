"""Formal source-only HiCache prediction orchestration."""

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
from typing import Any

from ...common.io import write_json
from ..planning.specs import ModelRunRequest
from ..types import CacheStatePredictionRef, ModelRunResult, ModelRunSpec
from ..validations.base_dag.preflight import DagTracePreflightCheck
from ..validations.hicache.preflight.state_input_preflight import HiCacheStateInputPreflightCheck
from .ledger import predicted_aggregates


@dataclass(frozen=True)
class HiCachePredictionRequest:
    """Predict target DAG effects and costs without reading target observations."""

    name = "hicache_prediction"

    def preflight_checks(self) -> tuple[type, ...]:
        return (DagTracePreflightCheck, HiCacheStateInputPreflightCheck)

    def build_model_run_requests(self, context: Any) -> list[ModelRunRequest]:
        predictions = [
            CacheStatePredictionRef(source=source, target=target)
            for source in context.runs
            for target in context.options.target_configs
        ]
        if context.options.max_predictions > 0:
            predictions = predictions[: context.options.max_predictions]
        if not predictions:
            raise ValueError("HiCache prediction produced no source/target requests")
        return self.requests_for(predictions)

    @staticmethod
    def requests_for(predictions: list[CacheStatePredictionRef]) -> list[ModelRunRequest]:
        return [
            ModelRunRequest(
                source_profile=prediction.source,
                target_config=prediction.target,
                prediction=prediction,
            )
            for prediction in predictions
        ]

    def analyze(
        self,
        context: Any,
        specs: list[ModelRunSpec],
        results: dict[str, ModelRunResult],
    ) -> dict[str, Any]:
        rows: list[dict[str, Any]] = []
        progress = context.reporter.start_stage(
            self.name,
            len(specs),
            "source-only Direct I/O/control",
            unit="prediction",
        )
        for spec in specs:
            row = self.build_row(results[spec.run_id])
            rows.append(row)
            if context.options.artifact_policy.keep_debug_artifacts:
                write_json(context.artifacts.debug_row_path(spec.run_id), row)
            progress.advance(self.progress_metrics(rows))
        summary = self.summarize(context, rows)
        progress.finish(str(summary["status"]), self.summary_text(summary))
        return summary

    @staticmethod
    def build_row(result: ModelRunResult) -> dict[str, Any]:
        spec = result.spec
        predicted, prediction_errors = predicted_aggregates(result)
        run_summary = result.artifacts.load_if_present(result.artifacts.run_summary_json)
        patch = _dict_field(_dict_field(run_summary, "module_results"), "hicache_dag_patch")
        validation = _dict_field(patch, "validation")
        resources = _dict_field(patch, "io_resources")

        structure_blockers: list[str] = []
        if result.skipped:
            structure_blockers.append(result.skip_reason or "skipped")
        if result.return_code != 0:
            structure_blockers.append("model_command_failed")
        structure_blockers.extend(prediction_errors)
        if validation.get("status") != "ready":
            structure_blockers.append("dag_patch_validation_not_ready")
        if resources.get("status") != "ready":
            structure_blockers.append("io_resources_not_ready")
        if patch.get("topology_valid") is not True:
            structure_blockers.append("topology_invalid")
        if patch.get("prefill_effect_status") != "deferred":
            structure_blockers.append("phase_effect_not_deferred")

        cost_blockers: list[str] = []
        if patch.get("status") not in {"applied", "no_mutation_required"}:
            cost_blockers.append("dag_patch_not_applied")
        cost_blockers.extend(f"dag_patch_apply:{value}" for value in sorted(patch.get("blocker_counts") or {}))

        records = [
            record
            for aggregate in (predicted.get("by_kind") or {}).values()
            if isinstance(aggregate, dict)
            for record in aggregate.get("records") or []
            if isinstance(record, dict)
        ]
        missing_mapping = sum(
            record.get("source_carrier_state") == "present" and not record.get("source_io_operation_record_ids")
            for record in records
        )
        if missing_mapping:
            structure_blockers.append("source_carrier_record_mapping_missing")

        structure_blockers = sorted(set(structure_blockers))
        cost_blockers = sorted(set(cost_blockers))
        structure_ready = not structure_blockers
        return {
            "model_run_id": spec.run_id,
            "pair_id": spec.prediction.label,
            "workload_id": spec.prediction.input_id,
            "source_run_id": spec.source_profile.run_id,
            "source_config_id": spec.source_profile.config_id,
            "target_config": spec.target_config.label,
            "target_config_path": str(spec.target_config.source_path) if spec.target_config.source_path else None,
            "is_self": spec.prediction.is_self,
            "status": "READY"
            if structure_ready and not cost_blockers
            else ("STRUCTURE_READY" if structure_ready else "NOT_READY"),
            "structure_ready": structure_ready,
            "structure_blockers": structure_blockers,
            "cost_blockers": cost_blockers,
            "prefill_decode_excluded": patch.get("prefill_effect_status") == "deferred",
            "target_predicted": predicted,
            "source_record_mapping": {
                "predicted_record_count": len(records),
                "source_carrier_present_count": sum(
                    record.get("source_carrier_state") == "present" for record in records
                ),
                "source_mapped_record_count": sum(
                    bool(record.get("source_io_operation_record_ids")) for record in records
                ),
                "target_created_record_count": sum(
                    record.get("source_carrier_state") == "absent" for record in records
                ),
                "missing_required_mapping_count": missing_mapping,
                "mapping_complete": missing_mapping == 0,
            },
        }

    @staticmethod
    def progress_metrics(rows: list[dict[str, Any]]) -> dict[str, Any]:
        return {
            "prediction-ready": f"{sum(row['structure_ready'] for row in rows)}/{len(rows)}",
            "cost-ready": f"{sum(row['status'] == 'READY' for row in rows)}/{len(rows)}",
        }

    @staticmethod
    def summarize(context: Any, rows: list[dict[str, Any]]) -> dict[str, Any]:
        ready_count = sum(row.get("status") == "READY" for row in rows)
        blockers: Counter[str] = Counter()
        for row in rows:
            blockers.update(str(value) for value in row.get("structure_blockers") or [])
            blockers.update(str(value) for value in row.get("cost_blockers") or [])
        return {
            "status": "READY" if rows and ready_count == len(rows) else "CHECK",
            "mode": "prediction",
            "dag_model_count": len(rows),
            "ready_count": ready_count,
            "structure_ready_count": sum(row.get("structure_ready") is True for row in rows),
            "blocker_counts": dict(sorted(blockers.items())),
            "source_profile_count": len({row.get("source_run_id") for row in rows}),
            "target_score_cell_count": 0,
            "prefill_decode_excluded": all(row.get("prefill_decode_excluded") is True for row in rows),
            "bounded_by_max_predictions": context.options.max_predictions > 0,
        }

    @staticmethod
    def summary_text(summary: dict[str, Any]) -> str:
        return (
            f"DAG models={summary['dag_model_count']} | "
            f"prediction-ready={summary['structure_ready_count']}/{summary['dag_model_count']} | "
            f"cost-ready={summary['ready_count']}/{summary['dag_model_count']}"
        )


def _dict_field(payload: dict[str, Any], name: str) -> dict[str, Any]:
    value = payload.get(name)
    return value if isinstance(value, dict) else {}
