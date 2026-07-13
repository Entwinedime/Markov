"""Validation request for the unpatched faithful-replay DAG."""

from __future__ import annotations

from .diagnostics import DagDiagnosticsValidation


class BaseDagValidation(DagDiagnosticsValidation):
    """Validate diagnostics for the original faithful-replay base DAG."""

    name = "base_dag"
    schema = "trace_sim.modeling_workflow.validation.base_dag.v2"
    progress_detail = "base DAG diagnostics"
