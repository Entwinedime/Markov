"""Adapt semantic model-run requirements to a self-contained runner config."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from ...modeling.run_config import ModelingOutputs, ModelingRunConfig
from ..io_model import HiCacheIoModel
from ..types import ModelRunSpec, TargetHiCacheConfig

if TYPE_CHECKING:
    from ..context import WorkflowOptions


def runner_config(spec: ModelRunSpec, options: WorkflowOptions) -> ModelingRunConfig:
    """Build one runner configuration from its sole source/target prediction."""

    diagnostics = options.diagnostics.keep_debug_artifacts
    cpp_config = {
        "threads": options.trace_threads,
        "file_threads": options.trace_file_threads,
    }
    window = spec.source.workload_window
    if window is not None:
        cpp_config["trace_window_start_us"] = window.start_ns // 1000
        cpp_config["trace_window_end_us"] = window.end_ns // 1000
        if diagnostics:
            cpp_config["actual_e2e_us"] = window.actual_e2e_ns // 1000

    return ModelingRunConfig(
        output_dir=spec.output_dir,
        profile_manifest=spec.source.manifest_path,
        cpp_config=cpp_config,
        outputs=ModelingOutputs(module_summary=diagnostics),
        backend_kind="validation" if diagnostics else "release",
        model_config_path=spec.output_dir / "cpp_model_config.json",
        cpu_service_cost=spec.cpu_service_cost,
        trace_channels=("torch", "ld_preload", "python_probe"),
    )


def hicache_model_config(
    target: TargetHiCacheConfig,
    *,
    source_prefetch_policy: str,
    source_target_same_config: bool,
    io_model: HiCacheIoModel,
) -> dict[str, Any]:
    """Build the narrow C++ HiCache config."""

    # Target input has already excluded costs and patch internals. Only the
    # shared model supplies costs. Prefetch policy was resolved at admission
    # so requirement planning and cost selection use the same execution branch.
    hicache_config = {
        "enabled": True,
        **target.fields,
        **io_model.narrow_config(
            target.fields["page_size"],
            target.fields["prefetch_policy"],
            target.fields.get("write_policy", "write_through"),
        ),
    }
    hicache_config["dag_patch"] = {
        "enabled": True,
        "source_target_same_config": source_target_same_config,
    }
    return {"hicache": hicache_config, "source_prefetch_policy": source_prefetch_policy}
