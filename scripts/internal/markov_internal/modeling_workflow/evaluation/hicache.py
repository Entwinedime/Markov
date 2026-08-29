"""Observed-target scoring layered on the formal HiCache predictor."""

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass, field
from typing import Any

from ...common.io import load_json, write_json
from ...modeling.workload import discover_workload_window
from ..io_model import HiCacheIoModel
from ..planning.profile_runs import group_runs_by_input
from ..planning.specs import ModelRunRequest
from ..prediction.hicache import HiCachePredictionRequest
from ..types import CacheStatePredictionRef, ModelRunResult, ModelRunSpec, ProfileRunRef, TargetHiCacheConfig
from ..validations.final_dag.shape_compare import compare_predicted_shape
from ..validations.final_dag.shape_oracle import extract_target_shape_oracle, patch_probe_contract_enabled


@dataclass(frozen=True)
class ObservedPredictionCell:
    prediction: CacheStatePredictionRef
    target: ProfileRunRef
    io_model: HiCacheIoModel | None


@dataclass
class HiCacheEvaluationRequest:
    """Run the core predictor, then compare its structure with target observations."""

    predictor: HiCachePredictionRequest = field(default_factory=HiCachePredictionRequest)
    _targets: dict[str, ProfileRunRef] = field(default_factory=dict, init=False)
    _shape_oracles: dict[str, dict[str, Any]] = field(default_factory=dict, init=False)

    name = "hicache_evaluation"

    def preflight_checks(self) -> tuple[type, ...]:
        return self.predictor.preflight_checks()

    def build_model_run_requests(self, context: Any) -> list[Any]:
        cells = observed_cells(
            context.runs,
            source_config_ids=context.options.source_config_ids,
            target_config_ids=context.options.target_config_ids,
            base_io_models=dict(context.options.base_io_models),
            max_predictions=context.options.max_predictions,
        )
        self._targets = {cell.prediction.label: cell.target for cell in cells}
        return [
            ModelRunRequest(
                source_profile=cell.prediction.source,
                target_config=cell.prediction.target,
                prediction=cell.prediction,
                hicache_io_model=cell.io_model,
            )
            for cell in cells
        ]

    def analyze(
        self,
        context: Any,
        specs: list[ModelRunSpec],
        results: dict[str, ModelRunResult],
    ) -> dict[str, Any]:
        rows: list[dict[str, Any]] = []
        progress = context.reporter.start_stage(self.name, len(specs), "target score only", unit="cell")
        for spec in specs:
            result = results[spec.run_id]
            row = self.predictor.build_row(result)
            target = self._targets[spec.prediction.label]
            score = self._shape_score(context, result, target)
            row.update(
                target_run_id=target.run_id,
                target_config_id=target.config_id,
                target_observation_used=True,
                structure_exact=score["acceptance_ready"],
                shape_score=score,
                score_blockers=[] if score["acceptance_ready"] is True else ["predicted_target_structure_mismatch"],
            )
            if row["score_blockers"]:
                row["status"] = "STRUCTURE_READY" if row["structure_ready"] else "NOT_READY"
            rows.append(row)
            if context.options.artifact_policy.keep_debug_artifacts:
                write_json(context.artifacts.debug_row_path(spec.run_id), row)
            progress.advance(self._progress_metrics(rows))
        summary = self._summary(context, rows)
        progress.finish(str(summary["status"]), self._summary_text(summary))
        return summary

    def _shape_score(self, context: Any, result: ModelRunResult, target: ProfileRunRef) -> dict[str, Any]:
        oracle = self._shape_oracle(target)
        comparison = compare_predicted_shape(
            result.artifacts.load_if_present(result.artifacts.model_summary_json),
            oracle,
        )
        return {
            "ready": oracle.get("ready"),
            "acceptance_ready": comparison.get("acceptance_ready"),
            "acceptance_mismatch_count": int(comparison.get("acceptance_mismatch_count") or 0),
            "diagnostic_exact": comparison.get("diagnostic_exact"),
            "mismatch_count": int(comparison.get("mismatch_count") or 0),
            "schedule_sensitive_count": int(comparison.get("schedule_sensitive_count") or 0),
        }

    def _shape_oracle(self, target: ProfileRunRef) -> dict[str, Any]:
        cached = self._shape_oracles.get(target.run_id)
        if cached is not None:
            return cached
        manifest = load_json(target.manifest_path) if target.manifest_path.is_file() else {}
        window = discover_workload_window({}, target.manifest_path)
        oracle = extract_target_shape_oracle(
            target.python_probe_files,
            patch_probe_contract_ready=patch_probe_contract_enabled(manifest if isinstance(manifest, dict) else {}),
            window_start_us=window.start_ns // 1000 if window is not None else None,
            window_end_us=window.end_ns // 1000 if window is not None else None,
        )
        self._shape_oracles[target.run_id] = oracle
        return oracle

    def _summary(self, context: Any, rows: list[dict[str, Any]]) -> dict[str, Any]:
        summary = self.predictor.summarize(context, rows)
        score_blockers: Counter[str] = Counter(blocker for row in rows for blocker in row.get("score_blockers") or [])
        all_blockers = Counter(summary["blocker_counts"])
        all_blockers.update(score_blockers)
        exact_count = sum(row.get("structure_exact") is True for row in rows)
        ready_count = sum(row.get("status") == "READY" for row in rows)
        summary.update(
            status="READY" if rows and ready_count == len(rows) else "CHECK",
            mode="evaluation",
            ready_count=ready_count,
            structure_score_cell_count=len(rows),
            structure_exact_count=exact_count,
            blocker_counts=dict(sorted(all_blockers.items())),
            target_score_cell_count=len({row["target_run_id"] for row in rows}),
            target_observation_used_for_parameters=False,
            target_e2e_used=False,
        )
        return summary

    @staticmethod
    def _progress_metrics(rows: list[dict[str, Any]]) -> dict[str, Any]:
        metrics = HiCachePredictionRequest.progress_metrics(rows)
        metrics["structure-score"] = f"{sum(row['structure_exact'] is True for row in rows)}/{len(rows)}"
        return metrics

    @staticmethod
    def _summary_text(summary: dict[str, Any]) -> str:
        return (
            HiCachePredictionRequest.summary_text(summary)
            + f" | structure-score={summary['structure_exact_count']}/{summary['structure_score_cell_count']}"
        )


def observed_cells(
    runs: list[ProfileRunRef],
    *,
    source_config_ids: frozenset[str],
    target_config_ids: frozenset[str],
    base_io_models: dict[str, HiCacheIoModel],
    max_predictions: int = 0,
) -> list[ObservedPredictionCell]:
    """Build a data-driven score matrix without fixed config or workload counts."""

    source_configs = source_config_ids or frozenset(base_io_models)
    if not source_configs:
        raise ValueError("evaluation requires --source-configs or at least one --base-io-model")
    if not base_io_models and len(source_configs) != 1:
        raise ValueError("multiple evaluation sources require one --base-io-model per source config")
    missing_models = source_configs - base_io_models.keys() if base_io_models else set()
    if missing_models:
        raise ValueError(f"evaluation has no --base-io-model for sources: {sorted(missing_models)}")
    cells: list[ObservedPredictionCell] = []
    for by_config in group_runs_by_input(runs).values():
        for source_config in sorted(source_configs):
            source = by_config.get(source_config)
            if source is None:
                continue
            for target in by_config.values():
                if target.config_id == source_config or (
                    target_config_ids and target.config_id not in target_config_ids
                ):
                    continue
                prediction = CacheStatePredictionRef(source=source, target=TargetHiCacheConfig.from_profile(target))
                cells.append(
                    ObservedPredictionCell(
                        prediction=prediction,
                        target=target,
                        io_model=base_io_models.get(source_config),
                    )
                )
    cells.sort(key=lambda cell: cell.prediction.label)
    if not cells:
        raise ValueError("evaluation selected no source-to-target cells")
    return cells[:max_predictions] if max_predictions > 0 else cells
