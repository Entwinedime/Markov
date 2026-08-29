"""Workflow preflight check for HiCache state-model inputs."""

from __future__ import annotations

from typing import Any

from ....preflight import PreflightCheck
from .state_input_preflight_report import build_state_input_preflight_report
from ....progress import count_text


class HiCacheStateInputPreflightCheck(PreflightCheck):
    """Audit facts and oracle evidence required by HiCache validations."""

    name = "hicache_state_inputs"
    detail_fields = ("runs",)

    def progress_text(self, summary: dict[str, Any]) -> str:
        """Render HiCache workflow-ready inputs against the selected run count."""

        return "hicache " + count_text(summary.get("workflow_input_ready_count"), summary.get("run_count"))

    def run(self, context: Any) -> dict[str, Any]:
        """Execute the HiCache input audit in the shared artifact layout."""

        report = build_state_input_preflight_report(
            context.runs,
            audit_dir=context.artifacts.preflight_dir / self.name,
            retain_details=context.options.artifact_policy.keep_debug_artifacts,
        )
        return {
            "check": self.name,
            "ready": report.get("workflow_input_ready"),
            "run_count": report.get("run_count"),
            "workflow_input_ready_count": report.get("workflow_input_ready_count"),
            "state_model_input_ready_count": report.get("state_model_input_ready_count"),
            "artifact_ready_count": report.get("artifact_ready_count"),
            "runs": report.get("runs", []),
        }
