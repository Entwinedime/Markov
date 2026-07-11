"""Compose C++ HiCache outputs into container-side validation artifacts."""

from __future__ import annotations

from typing import Any

from markov_internal.common.io import write_json
from markov_internal.modeling.run_config import ModelingRunConfig
from markov_internal.modeling.trace_inputs import trace_paths_for_run
from markov_internal.modeling.workload import discover_workload_window

from .prediction import write_predicted_state_trace
from .validation import build_validation, write_recommended_cpp_config


def write_validation_artifacts(
    run: ModelingRunConfig,
    prediction: dict[str, Any],
    run_summary: dict[str, Any],
) -> None:
    """Derive requested Python validation artifacts from completed C++ outputs.

    This adapter is injected by the container entry point after the generic runner
    succeeds. It may consume Debug summaries and oracle traces, but it cannot alter
    C++ model state or the trace channels admitted to the backend.
    """

    trace_paths = trace_paths_for_run(run.raw, run.profile_manifest)
    module_summary = run.output_dir / "model_summary.json"
    predicted_state_trace = write_predicted_state_trace(module_summary, run.output_dir)
    validation = build_validation(
        run.mode,
        prediction,
        run_summary,
        discover_workload_window(run.input_config, run.profile_manifest),
        trace_paths,
        run.raw,
        module_summary,
        predicted_state_trace,
        list(run.hicache_oracle_traces),
    )
    recommended_config = write_recommended_cpp_config(validation, run.output_dir)
    hicache_state = validation.get("hicache_state")
    if recommended_config is not None and isinstance(hicache_state, dict):
        hicache_state["recommended_hicache_cpp_model_config_path"] = str(recommended_config)
    write_json(run.output_dir / "validation.json", validation)
