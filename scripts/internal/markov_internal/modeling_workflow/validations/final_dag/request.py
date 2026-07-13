"""Prediction-cell validation for the active DAG after HiCache patching."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ....common.io import load_json, write_json
from ...progress import count_text
from ...types import ModelOutputRequirement, ModelRunResult, ModelRunSpec, ValidationSummary
from ..base_dag.preflight import DagTracePreflightCheck
from ..hicache.preflight.state_input_preflight import HiCacheStateInputPreflightCheck
from ..registry import PredictionValidation, RowValidation, count_blockers, readiness_status
from .shape_oracle import compare_predicted_shape, extract_target_shape_oracle, patch_probe_contract_enabled


class FinalDagValidation(PredictionValidation, RowValidation):
    """Validate one post-module active DAG per source-target prediction cell."""

    name = "final_dag"
    progress_detail = "final DAG prediction cells"
    progress_unit = "prediction"
    cache_state_output_requirements = frozenset(
        {
            ModelOutputRequirement.HICACHE_VALIDATION,
            ModelOutputRequirement.HICACHE_DAG_PATCH,
            ModelOutputRequirement.DAG_ANALYSIS,
        }
    )

    def __init__(self) -> None:
        self._target_shape_oracles: dict[str, tuple[dict[str, Any], Path]] = {}
        self._self_shape_evidence: dict[str, bool] = {}
        self._source_baseline_e2e_us: dict[str, int] = {}

    def analyze(
        self,
        context: Any,
        specs: list[ModelRunSpec],
        results: dict[str, ModelRunResult],
    ) -> ValidationSummary:
        """Precompute target self-shape evidence before evaluating cross cells."""

        self._self_shape_evidence = {}
        self._source_baseline_e2e_us = {}
        for spec in specs:
            if spec.mode != "faithful_replay":
                continue
            result = results.get(spec.run_id)
            if result is None or result.return_code != 0 or result.skipped:
                continue
            run_summary = result.artifacts.load_if_present(result.artifacts.run_summary_json)
            simulated_e2e = run_summary.get("simulated_e2e_us")
            if isinstance(simulated_e2e, int) and not isinstance(simulated_e2e, bool):
                self._source_baseline_e2e_us[spec.source_profile.run_id] = simulated_e2e
        for spec in self.selected_specs(specs):
            prediction = spec.prediction
            if prediction is None or not prediction.is_self:
                continue
            result = results[spec.run_id]
            oracle, _ = self._target_shape_oracle(context, result)
            model_summary = result.artifacts.load_if_present(result.artifacts.model_summary_json)
            comparison = compare_predicted_shape(
                model_summary,
                oracle,
                sample_limit=context.options.sample_limit,
                is_self=True,
                alternate_evidence_ready=True,
            )
            if spec.target_profile is not None:
                # Readiness limitations remain non-exact, but do not invalidate the
                # schedule-invariant self evidence used by cross predictions.
                self._self_shape_evidence[spec.target_profile.run_id] = bool(
                    comparison.get("ready") is True and comparison.get("acceptance_ready") is True
                )
        return super().analyze(context, specs, results)

    def preflight_checks(self) -> tuple[type, ...]:
        """Require both a complete source DAG and valid HiCache state inputs."""

        return (DagTracePreflightCheck, HiCacheStateInputPreflightCheck)

    def build_row(self, context: Any, result: ModelRunResult) -> dict[str, Any]:
        """Build one row from the active graph and patch-module artifacts."""

        artifacts = result.artifacts
        run_summary = artifacts.load_if_present(artifacts.run_summary_json)
        dag_quality = artifacts.load_if_present(artifacts.dag_quality_json)
        dag_analysis = artifacts.load_if_present(artifacts.dag_analysis_json)
        validation = artifacts.load_if_present(artifacts.validation_json)
        model_summary = artifacts.load_if_present(artifacts.model_summary_json)
        module_results = (
            run_summary.get("module_results") if isinstance(run_summary.get("module_results"), dict) else {}
        )
        patch = module_results.get("hicache_dag_patch") if isinstance(module_results, dict) else None
        dag_build = dag_quality.get("dag_build") if isinstance(dag_quality.get("dag_build"), dict) else {}
        analysis_blockers = dag_analysis.get("blockers") if isinstance(dag_analysis.get("blockers"), list) else []

        blockers: list[str] = []
        if result.skipped:
            blockers.append(result.skip_reason or "skipped")
        if result.return_code != 0:
            blockers.append("model_command_failed")
        for path in (
            artifacts.run_summary_json,
            artifacts.validation_json,
            artifacts.dag_quality_json,
            artifacts.dag_analysis_json,
            artifacts.model_summary_json,
        ):
            if not path.is_file() and not result.skipped:
                blockers.append(f"missing_artifact:{path.name}")
        if not isinstance(patch, dict):
            if not result.skipped:
                blockers.append("missing_hicache_dag_patch_result")
        elif patch.get("topology_valid") is False:
            blockers.append("final_dag_topology_invalid")
        source_attribution = (
            patch.get("source_attribution")
            if isinstance(patch, dict) and isinstance(patch.get("source_attribution"), dict)
            else {}
        )
        shadow_rewrite = (
            patch.get("shadow_rewrite")
            if isinstance(patch, dict) and isinstance(patch.get("shadow_rewrite"), dict)
            else {}
        )
        boundary_validation = (
            patch.get("boundary_validation")
            if isinstance(patch, dict) and isinstance(patch.get("boundary_validation"), dict)
            else {}
        )
        applied_validation = (
            patch.get("applied_validation")
            if isinstance(patch, dict) and isinstance(patch.get("applied_validation"), dict)
            else {}
        )
        io_resources = (
            patch.get("io_resources") if isinstance(patch, dict) and isinstance(patch.get("io_resources"), dict) else {}
        )
        if isinstance(patch, dict):
            if patch.get("prefill_effect_status") != "deferred":
                blockers.append("hicache_prefill_effect_status_not_deferred")
            if source_attribution.get("status") != "ready":
                blockers.append("hicache_source_attribution_not_ready")
            if shadow_rewrite.get("status") != "ready":
                blockers.append("hicache_shadow_rewrite_not_ready")
            if boundary_validation.get("status") != "ready":
                blockers.append("hicache_boundary_validation_not_ready")
            if applied_validation.get("status") != "ready":
                blockers.append("hicache_applied_validation_not_ready")
            if patch.get("status") not in {"applied", "no_mutation_required"}:
                blockers.append("hicache_patch_not_applied")
            apply_blockers = patch.get("apply_blockers")
            if isinstance(apply_blockers, dict):
                blockers.extend(f"hicache_patch_apply:{name}" for name in sorted(apply_blockers))

        prediction = result.spec.prediction
        is_self = bool(prediction is not None and prediction.is_self)
        target_run_id = result.spec.target_profile.run_id if result.spec.target_profile else ""
        alternate_evidence_ready = bool(
            not is_self
            and self._self_shape_evidence.get(target_run_id, False)
            and applied_validation.get("status") == "ready"
            and isinstance(patch, dict)
            and patch.get("status") in {"applied", "no_mutation_required"}
        )
        shape_oracle, shape_oracle_path = self._target_shape_oracle(context, result)
        shape_comparison = compare_predicted_shape(
            model_summary,
            shape_oracle,
            sample_limit=context.options.sample_limit,
            is_self=is_self,
            alternate_evidence_ready=alternate_evidence_ready,
        )
        shape_comparison_path = self._shape_comparison_path(context, result)
        write_json(shape_comparison_path, shape_comparison)
        if shape_comparison.get("ready") is not True:
            blockers.append("hicache_target_shape_oracle_not_ready")
        elif shape_comparison.get("status") == "alternate_evidence_missing":
            blockers.append("hicache_schedule_sensitive_alternate_evidence_missing")
        elif shape_comparison.get("acceptance_ready") is not True:
            blockers.append("hicache_target_shape_mismatch")
        blockers.extend(str(blocker) for blocker in analysis_blockers)

        hicache_validation = (
            validation.get("hicache_state") if isinstance(validation.get("hicache_state"), dict) else {}
        )
        critical_path = dag_analysis.get("critical_path") if isinstance(dag_analysis.get("critical_path"), dict) else {}
        simulated_e2e_us = run_summary.get("simulated_e2e_us")
        source_baseline_e2e_us = self._source_baseline_e2e_us.get(result.spec.source_profile.run_id)
        simulated_patch_delta_us = (
            simulated_e2e_us - source_baseline_e2e_us
            if isinstance(simulated_e2e_us, int)
            and not isinstance(simulated_e2e_us, bool)
            and source_baseline_e2e_us is not None
            else None
        )
        return {
            "model_run_id": result.spec.run_id,
            "label": result.spec.label,
            "input_id": result.spec.source_profile.input_id,
            "source_run_id": result.spec.source_profile.run_id,
            "source_config_id": result.spec.source_profile.config_id,
            "target_run_id": result.spec.target_profile.run_id if result.spec.target_profile else None,
            "target_config_id": result.spec.target_profile.config_id if result.spec.target_profile else None,
            "is_self": is_self,
            "output_dir": str(result.spec.output_dir),
            "return_code": result.return_code,
            "skipped": result.skipped,
            "skip_reason": result.skip_reason or None,
            "ready": not blockers,
            "blockers": blockers,
            "final_dag_source": "cache_state_prediction_active_graph",
            "trace_channels": list(result.spec.trace_channels),
            "patch_status": patch.get("status") if isinstance(patch, dict) else None,
            "prefill_effect_status": patch.get("prefill_effect_status") if isinstance(patch, dict) else None,
            "prefetch_readiness_status": (patch.get("prefetch_readiness_status") if isinstance(patch, dict) else None),
            "patch_plan_id": patch.get("plan_id") if isinstance(patch, dict) else None,
            "patch_mutation_count": int(patch.get("mutation_count") or 0) if isinstance(patch, dict) else 0,
            "patch_applied": bool(isinstance(patch, dict) and int(patch.get("mutation_count") or 0) > 0),
            "patch_apply_blockers": patch.get("apply_blockers") if isinstance(patch, dict) else None,
            "topology_valid": patch.get("topology_valid") if isinstance(patch, dict) else None,
            "source_attribution_status": source_attribution.get("status"),
            "decision_count": int(source_attribution.get("decision_count") or 0),
            "source_attributed_count": int(source_attribution.get("attributed_count") or 0),
            "source_unresolved_count": int(source_attribution.get("unresolved_count") or 0),
            "shadow_rewrite_status": shadow_rewrite.get("status"),
            "shadow_rewrite_ready_count": int(shadow_rewrite.get("ready_count") or 0),
            "shadow_rewrite_rejected_count": int(shadow_rewrite.get("rejected_count") or 0),
            "shadow_topology_valid": shadow_rewrite.get("shadow_topology_valid"),
            "boundary_validation_status": boundary_validation.get("status"),
            "boundary_validation_ready_count": int(boundary_validation.get("ready_count") or 0),
            "applied_validation_status": applied_validation.get("status"),
            "applied_validation_ready_count": int(applied_validation.get("ready_count") or 0),
            "applied_validation_blockers": applied_validation.get("blocker_counts"),
            "io_resource_status": io_resources.get("status"),
            "cost_ready_count": int(io_resources.get("cost_ready_count") or 0),
            "lane_dependency_count": int(io_resources.get("lane_dependency_count") or 0),
            "shape_oracle_ready": shape_oracle.get("ready"),
            "shape_oracle_status": shape_oracle.get("status"),
            "shape_exact": shape_comparison.get("exact"),
            "shape_acceptance_ready": shape_comparison.get("acceptance_ready"),
            "shape_diagnostic_exact": shape_comparison.get("diagnostic_exact"),
            "shape_mismatch_count": int(shape_comparison.get("mismatch_count") or 0),
            "shape_acceptance_mismatch_count": int(shape_comparison.get("acceptance_mismatch_count") or 0),
            "shape_invariant_mismatch_count": int(shape_comparison.get("invariant_mismatch_count") or 0),
            "shape_schedule_sensitive_mismatch_count": int(
                shape_comparison.get("schedule_sensitive_mismatch_count") or 0
            ),
            "shape_readiness_limitation_mismatch_count": int(
                shape_comparison.get("readiness_limitation_mismatch_count") or 0
            ),
            "schedule_sensitive_count": int(shape_comparison.get("schedule_sensitive_count") or 0),
            "alternate_evidence_required": shape_comparison.get("alternate_evidence_required"),
            "alternate_evidence_ready": shape_comparison.get("alternate_evidence_ready"),
            "shape_oracle_blockers": shape_oracle.get("blocker_counts"),
            "shape_oracle_path": str(shape_oracle_path),
            "shape_comparison_path": str(shape_comparison_path),
            "state_model_fact_ready": hicache_validation.get("state_model_fact_ready"),
            "final_state_match": hicache_validation.get("final_state_match"),
            "node_count": run_summary.get("node_count") or dag_build.get("node_count"),
            "edge_count": run_summary.get("edge_count") or dag_build.get("edge_count"),
            "synthetic_node_count": run_summary.get("synthetic_node_count") or dag_build.get("synthetic_node_count"),
            "simulated_source_e2e_us": source_baseline_e2e_us,
            "simulated_e2e_us": simulated_e2e_us,
            "simulated_patch_delta_us": simulated_patch_delta_us,
            "critical_path_synthetic_effect_count": int(critical_path.get("synthetic_effect_count") or 0),
            "critical_path_synthetic_effect_us": int(critical_path.get("synthetic_effect_duration_us") or 0),
            "run_summary_path": str(artifacts.run_summary_json),
            "dag_quality_path": str(artifacts.dag_quality_json),
            "dag_analysis_path": str(artifacts.dag_analysis_json),
        }

    def running_metrics(self, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """Return running readiness and materialized-patch counters."""

        return {
            "ready": count_text(sum(1 for row in rows if row.get("ready")), len(rows)),
            "patched": count_text(sum(1 for row in rows if row.get("patch_applied")), len(rows)),
        }

    def build_summary(self, context: Any, rows: list[dict[str, Any]]) -> dict[str, Any]:
        """Aggregate final-DAG prediction readiness without using raw E2E as a gate."""

        prediction_count = len(rows)
        ready_count = sum(1 for row in rows if row.get("ready") is True)
        skipped_count = sum(1 for row in rows if row.get("skipped"))
        error_count = sum(1 for row in rows if row.get("return_code") not in (0, None))
        return {
            "schema": "trace_sim.modeling_workflow.validation.final_dag.v2",
            "validation": self.name,
            "status": readiness_status(prediction_count, error_count, ready_count),
            "prediction_count": prediction_count,
            "ready_count": ready_count,
            "patch_applied_count": sum(1 for row in rows if row.get("patch_applied") is True),
            "prefill_deferred_count": sum(1 for row in rows if row.get("prefill_effect_status") == "deferred"),
            "prefetch_readiness_limited_count": sum(
                1 for row in rows if row.get("prefetch_readiness_status") == "payload_only_control_pipeline_unmodeled"
            ),
            "cost_ready_count": sum(int(row.get("cost_ready_count") or 0) for row in rows),
            "lane_dependency_count": sum(int(row.get("lane_dependency_count") or 0) for row in rows),
            "patch_delta_available_count": sum(1 for row in rows if row.get("simulated_patch_delta_us") is not None),
            "critical_path_synthetic_effect_count": sum(
                int(row.get("critical_path_synthetic_effect_count") or 0) for row in rows
            ),
            "critical_path_synthetic_effect_us": sum(
                int(row.get("critical_path_synthetic_effect_us") or 0) for row in rows
            ),
            "source_attribution_ready_count": sum(1 for row in rows if row.get("source_attribution_status") == "ready"),
            "shadow_rewrite_ready_count": sum(1 for row in rows if row.get("shadow_rewrite_status") == "ready"),
            "boundary_validation_ready_count": sum(
                1 for row in rows if row.get("boundary_validation_status") == "ready"
            ),
            "applied_validation_ready_count": sum(1 for row in rows if row.get("applied_validation_status") == "ready"),
            "shape_oracle_ready_count": sum(1 for row in rows if row.get("shape_oracle_ready") is True),
            "shape_exact_count": sum(1 for row in rows if row.get("shape_exact") is True),
            "shape_acceptance_ready_count": sum(1 for row in rows if row.get("shape_acceptance_ready") is True),
            "shape_diagnostic_exact_count": sum(1 for row in rows if row.get("shape_diagnostic_exact") is True),
            "shape_readiness_limitation_mismatch_count": sum(
                int(row.get("shape_readiness_limitation_mismatch_count") or 0) for row in rows
            ),
            "schedule_sensitive_effect_count": sum(int(row.get("schedule_sensitive_count") or 0) for row in rows),
            "alternate_evidence_ready_count": sum(
                1
                for row in rows
                if row.get("alternate_evidence_required") is True and row.get("alternate_evidence_ready") is True
            ),
            "alternate_evidence_missing_count": sum(
                1
                for row in rows
                if row.get("alternate_evidence_required") is True and row.get("alternate_evidence_ready") is not True
            ),
            "topology_valid_count": sum(1 for row in rows if row.get("topology_valid") is True),
            "state_model_fact_ready_count": sum(1 for row in rows if row.get("state_model_fact_ready") is True),
            "final_state_match_count": sum(1 for row in rows if row.get("final_state_match") is True),
            "skipped_count": skipped_count,
            "error_count": error_count,
            "blocker_counts": count_blockers(rows, "blockers"),
            "final_dag_source": "cache_state_prediction_active_graph",
            "raw_e2e_is_gate": False,
        }

    def _target_shape_oracle(
        self,
        context: Any,
        result: ModelRunResult,
    ) -> tuple[dict[str, Any], Path]:
        """Build and cache one independent target-actual oracle per profile run."""

        target = result.spec.target_profile
        if target is None:
            path = context.artifacts.validations_dir / self.name / "target_shape_oracles" / "missing_target.json"
            return {"status": "not_ready", "ready": False, "blocker_counts": {"missing_target_profile": 1}}, path
        cached = self._target_shape_oracles.get(target.run_id)
        if cached is not None:
            return cached
        manifest = load_json(target.manifest_path) if target.manifest_path.is_file() else {}
        oracle = extract_target_shape_oracle(
            target.python_probe_files,
            target_run_id=target.run_id,
            target_config_id=target.config_id,
            input_id=target.input_id,
            patch_probe_contract_ready=patch_probe_contract_enabled(manifest if isinstance(manifest, dict) else {}),
        )
        path = context.artifacts.validations_dir / self.name / "target_shape_oracles" / f"{target.run_id}.json"
        write_json(path, oracle)
        self._target_shape_oracles[target.run_id] = (oracle, path)
        return oracle, path

    def _shape_comparison_path(self, context: Any, result: ModelRunResult) -> Path:
        """Return the per-cell target-shape comparison artifact path."""

        return context.artifacts.validations_dir / self.name / "shape_comparisons" / f"{result.spec.run_id}.json"

    def summary_text(self, summary: dict[str, Any]) -> str:
        """Render compact final-DAG prediction progress."""

        return (
            f"{summary['prediction_count']} predictions | "
            f"ready {summary['ready_count']}/{summary['prediction_count']} | "
            f"patched {summary['patch_applied_count']}/{summary['prediction_count']}"
        )

    def validation_summary(
        self,
        context: Any,
        rows: list[dict[str, Any]],
        summary: dict[str, Any],
    ) -> ValidationSummary:
        """Convert final-DAG counts to the workflow summary contract."""

        return ValidationSummary(
            name=self.name,
            status=summary["status"],
            selected_run_count=len(rows),
            ready_count=int(summary["ready_count"]),
            skipped_count=int(summary["skipped_count"]),
            blocker_counts=summary["blocker_counts"],
            artifact_paths={"summary": str(context.artifacts.validation_summary_path(self.name))},
            payload=summary,
        )
