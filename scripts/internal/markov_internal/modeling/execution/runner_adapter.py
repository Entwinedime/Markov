"""Adapt semantic model-run requirements to a self-contained runner config."""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING, Any

from markov_internal.modeling.run_config import ModelingOutputs, ModelingRunConfig
from markov_internal.common.paths import repo_relative_path
from markov_internal.modeling.types import ModelRunSpec, TargetHiCacheConfig

if TYPE_CHECKING:
    from markov_internal.modeling.context import WorkflowOptions


def runner_config(spec: ModelRunSpec, options: WorkflowOptions) -> ModelingRunConfig:
    """Build one runner configuration from its sole source/target prediction."""

    diagnostics = options.diagnostics.keep_debug_artifacts
    window = spec.source.workload_window
    return ModelingRunConfig(
        output_dir=spec.output_dir,
        profile_manifest=spec.source.manifest_path,
        outputs=ModelingOutputs(
            module_summary=diagnostics,
            source_replay_report=options.experiment_reports,
            structure_report=options.experiment_reports,
        ),
        threads=options.trace_threads,
        file_threads=options.trace_file_threads,
        trace_window_start_us=window.start_ns // 1000 if window else None,
        trace_window_end_us=window.end_ns // 1000 if window else None,
        actual_e2e_us=window.actual_e2e_ns // 1000 if window and diagnostics else None,
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
    cost_model_path: Path,
) -> dict[str, Any]:
    """Build the narrow C++ HiCache config."""

    # Target input has already excluded costs and patch internals. Only the
    # shared model supplies costs. Prefetch policy drives execution; it does
    # not select a measured waiting program.
    hicache_config = {
        "enabled": True,
        **target.fields,
        "cost_package": str(repo_relative_path(cost_model_path)),
    }
    hicache_config["dag_patch"] = {
        "enabled": True,
        "source_target_same_config": source_target_same_config,
    }
    return {"hicache": hicache_config, "source_prefetch_policy": source_prefetch_policy}
