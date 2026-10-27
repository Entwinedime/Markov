"""Preview declared preparation without opening traces or starting resources."""

from __future__ import annotations

from dataclasses import asdict
from typing import Any

from markov.artifacts.request import PredictionRequest


def plan_prediction(request: PredictionRequest) -> dict[str, Any]:
    """Describe known inputs; dynamic work and model coverage remain unexamined."""
    request.workload.stat()
    if request.base.manifest is not None:
        request.base.manifest.stat()
    if request.models is not None:
        request.models.stat()

    preparation = []
    if request.base.manifest is None:
        preparation.append("capture_base")
    if request.models is None:
        preparation.append("select_independent_cost_models")

    return {
        "status": "planned",
        "deployment": asdict(request.deployment),
        "base": {
            "manifest": str(request.base.manifest) if request.base.manifest is not None else None,
            "requires_capture": request.base.manifest is None,
        },
        "workload": str(request.workload),
        "targets": [item.name for item in request.targets],
        "models": str(request.models) if request.models is not None else None,
        "preparation": preparation,
        "not_evaluated": ["source_semantics", "target_work", "cost_coverage", "execution"],
        "execution": {
            "output": str(request.execution.output),
            "jobs": request.execution.jobs,
            "memory_limit_gib": request.execution.memory_limit_gib,
        },
    }
