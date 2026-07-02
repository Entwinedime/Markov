"""base DAG 验证请求。"""

from __future__ import annotations

from .diagnostics import DagDiagnosticsValidation


class BaseDagValidation(DagDiagnosticsValidation):
    """验证 faithful replay 构造出的原始 Base DAG。"""

    name = "base_dag"
    schema = "trace_sim.modeling_workflow.validation.base_dag.v1"
    progress_detail = "base DAG diagnostics"
