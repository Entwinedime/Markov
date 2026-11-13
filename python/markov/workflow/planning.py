"""Preview declared preparation without opening traces or starting resources."""

from __future__ import annotations

from dataclasses import asdict
from typing import Any

from markov.artifacts.request import PredictionRequest
from markov.artifacts.workload import Workload
from markov.artifacts.performance import PerformanceData
from markov.workflow.capture import InferenceCapture


def plan_prediction(request: PredictionRequest) -> dict[str, Any]:
    """Describe known inputs; dynamic work and model coverage remain unexamined."""
    if request.deployment.framework != "sglang":
        raise ValueError(f"Prediction does not support framework {request.deployment.framework}")
    workload = Workload.load(request.workload)
    if request.base.manifest is not None:
        request.base.manifest.stat()
    if request.models is not None:
        request.models.stat()
    if request.database is not None and request.models is None and request.sampling_budget is None:
        request.database.stat()

    base_preview = InferenceCapture(request, workload=workload).preview(workload)
    costs = []
    if request.models is None and request.database is not None and request.database.is_file():
        store = PerformanceData(request.database, readonly=True)
        try:
            deployments = {target.deployment.tensor_parallel: target.deployment for target in request.targets}
            for deployment in deployments.values():
                selected = store.selected_models(asdict(deployment))
                costs.append({"deployment": asdict(deployment), "response_count": len(selected["models"]) if selected else 0})
        finally:
            store.close()

    preparation = []
    if base_preview["status"] not in ("declared", "matching_capture"):
        preparation.append("capture_or_reuse_base")
    if request.models is None:
        preparation.append(
            "prepare_shared_costs"
            if request.sampling_budget is not None
            else "export_maintained_models"
            if request.database is not None
            else "select_independent_cost_models"
        )

    return {
        "status": "planned",
        "deployment": asdict(request.deployment),
        "base": {
            "manifest": str(request.base.manifest) if request.base.manifest is not None else None,
            "requires_preparation": base_preview["status"] not in ("declared", "matching_capture"),
            "detail": request.execution.capture.detail if request.execution.capture is not None else None,
            "reuse": base_preview,
        },
        "workload": str(request.workload),
        "workload_size": {
            "requests": len(workload.requests),
            "formal_requests": len(workload.requests) - workload.formal_begin_index,
            "output_token_limit": sum(row.max_new_tokens for row in workload.requests),
            "input_tokens": sum(len(row.input_ids) for row in workload.requests)
            if all(row.input_ids is not None for row in workload.requests)
            else None,
        },
        "capture": asdict(request.execution.capture) if request.execution.capture is not None else None,
        "targets": [asdict(item) for item in request.targets],
        "models": str(request.models) if request.models is not None else None,
        "database": str(request.database) if request.database is not None else None,
        "sampling_budget": request.sampling_budget,
        "maintained_selections": costs,
        "preparation": preparation,
        "not_evaluated": [
            "source_semantics",
            "target_work",
            "cost_coverage",
            "execution",
        ],
        "execution": {
            "output": str(request.execution.output),
            "memory_limit_gib": request.execution.memory_limit_gib,
            "record_graph": request.execution.record_graph,
        },
    }
