"""HiCache state-model input preflight 检查。"""

from __future__ import annotations

from typing import Any

from ....preflight import PreflightCheck
from .state_input_preflight_report import build_state_input_preflight_report


class HiCacheStateInputPreflightCheck(PreflightCheck):
    """审计 HiCache state validation 需要的 facts 与 oracle evidence。"""

    name = "hicache_state_inputs"

    def run(self, context: Any) -> dict[str, Any]:
        """在统一 artifact 布局下执行 HiCache input preflight audit。"""

        selected = set(context.options.validations)
        report = build_state_input_preflight_report(
            context.runs,
            context.options.output_dir,
            audit_dir=context.artifacts.preflight_dir / self.name,
            summary_path=context.artifacts.preflight_dir / self.name / "summary.json",
            require_validation_evidence=bool({"hicache_final_state", "hicache_transition"} & selected),
            validate_diagnostic_coverage="hicache_transition" in selected,
            require_cross_config_contract=(
                bool({"hicache_final_state", "hicache_transition"} & selected)
                and "cross" in context.options.prediction_scope
            ),
            show_workload_sequence=context.options.show_workload_sequence,
        )
        return {
            "schema": "trace_sim.modeling_workflow.preflight.hicache_state_inputs.v1",
            "check": self.name,
            "ready": report.get("workflow_input_ready"),
            "run_count": report.get("run_count"),
            "workflow_input_ready_count": report.get("workflow_input_ready_count"),
            "state_model_input_ready_count": report.get("state_model_input_ready_count"),
            "artifact_ready_count": report.get("artifact_ready_count"),
            "input_workload_signatures": report.get("input_workload_signatures", {}),
            "runs": report.get("runs", []),
            "source_summary_path": str(context.artifacts.preflight_dir / self.name / "summary.json"),
        }
