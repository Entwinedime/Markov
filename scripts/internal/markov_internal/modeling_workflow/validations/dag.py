"""Shared request contract for per-profile DAG artifact validations."""

from __future__ import annotations

from typing import TYPE_CHECKING

from ..planning.specs import ModelRunRequest
from ..types import ModelOutputRequirement
from .base_dag.preflight import DagTracePreflightCheck
from .registry import RowValidation

if TYPE_CHECKING:
    from ..context import WorkflowContext


class DagArtifactValidation(RowValidation):
    """Request one reusable faithful-replay DAG analysis per selected profile."""

    progress_unit = "run"

    def preflight_checks(self) -> tuple[type[DagTracePreflightCheck], ...]:
        """Require complete trace-channel coverage before requesting DAG artifacts."""

        return (DagTracePreflightCheck,)

    def build_model_run_requests(self, context: WorkflowContext) -> list[ModelRunRequest]:
        """Request one deduplicatable faithful-replay run per selected profile."""

        return [
            ModelRunRequest(
                mode="faithful_replay",
                source_profile=run,
                target_profile=None,
                output_requirements=frozenset({ModelOutputRequirement.DAG_ANALYSIS}),
                validation_name=self.name,
            )
            for run in context.runs
        ]
