"""model run plan artifact 序列化。"""

from __future__ import annotations

from typing import Any

from ...common.io import write_json
from ..artifacts import WorkflowArtifactLayout
from ..types import ModelRunSpec, ProfileRunRef


def write_model_run_plan(
    artifacts: WorkflowArtifactLayout,
    runs: list[ProfileRunRef],
    specs: list[ModelRunSpec],
    *,
    selected_validations: tuple[str, ...],
) -> dict[str, Any]:
    """写出统一 workflow 的 model run plan artifact。"""

    payload: dict[str, Any] = {
        "schema": "trace_sim.modeling_workflow.model_run_plan.v1",
        "selected_validations": list(selected_validations),
        "run_count": len(runs),
        "model_run_count": len(specs),
        "runs": [
            {
                "run_id": run.run_id,
                "config_id": run.config_id,
                "input_id": run.input_id,
                "input_class": run.input_class,
                "manifest_path": str(run.manifest_path),
                "run_dir": str(run.run_dir),
                "config_path": str(run.config_path),
                "python_probe_files": [str(path) for path in run.python_probe_files],
            }
            for run in runs
        ],
        "model_runs": [model_run_spec_payload(spec) for spec in specs],
    }
    write_json(artifacts.plan_path, payload)
    return payload


def model_run_spec_payload(spec: ModelRunSpec) -> dict[str, Any]:
    """把 ModelRunSpec 转成 JSON payload。"""

    payload: dict[str, Any] = {
        "run_id": spec.run_id,
        "mode": spec.mode,
        "label": spec.label,
        "source_run_id": spec.source_profile.run_id,
        "source_config_id": spec.source_profile.config_id,
        "input_id": spec.source_profile.input_id,
        "target_run_id": spec.target_profile.run_id if spec.target_profile is not None else None,
        "target_config_id": spec.target_profile.config_id if spec.target_profile is not None else None,
        "output_requirements": sorted(requirement.value for requirement in spec.output_requirements),
        "validation_requests": list(spec.validation_requests),
        "output_dir": str(spec.output_dir),
        "skip_reason": spec.skip_reason or None,
        "trace_channels": list(spec.trace_channels),
    }
    if spec.prediction is not None:
        payload["prediction"] = {
            "is_self": spec.prediction.is_self,
            "source_config_id": spec.prediction.source.config_id,
            "target_config_id": spec.prediction.target.config_id,
        }
    return payload
