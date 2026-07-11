#!/usr/bin/env python3
"""Container-side orchestration for one C++ modeling run."""

from __future__ import annotations

import argparse
import sys
from collections.abc import Callable
from pathlib import Path
from typing import Any

from ..common.io import load_json, write_json
from ..common.paths import require_repo_path, running_in_modeling_container
from .backend import build_trace_graph_command, execute_trace_graph
from .cpp_config import write_cpp_model_config
from .run_config import ModelingRunConfig


ValidationArtifactWriter = Callable[[ModelingRunConfig, dict[str, Any], dict[str, Any]], None]


def require_modeling_container() -> None:
    """Reject direct use of the internal runner outside its modeling container."""

    if running_in_modeling_container():
        return
    raise SystemExit(
        "scripts/internal/entrypoints/model.py is container-internal; "
        "start workflows with python3 scripts/internal/entrypoints/modeling_workflow.py"
    )


def parse_args(argv: list[str] | None = None) -> Path:
    """Parse the single supported self-contained runner config argument."""

    parser = argparse.ArgumentParser(description="Run one generated C++ modeling config.")
    parser.add_argument("--config", required=True, help="self-contained modeling runner config")
    args = parser.parse_args(argv)
    config_path = require_repo_path(args.config)
    if not config_path.is_file():
        raise FileNotFoundError(f"missing config: {config_path}")
    return config_path


def run_from_config(
    config_path: Path,
    *,
    validation_artifact_writer: ValidationArtifactWriter | None = None,
) -> dict[str, Any]:
    """Run C++ and delegate optional validation artifacts to the composition root.

    The generic runner owns config validation, backend execution, and the compact
    prediction artifact. A caller requesting validation must inject the workflow-owned
    writer; this keeps the low-level modeling package independent of validation domains.
    """

    run = ModelingRunConfig.load(config_path)
    run.output_dir.mkdir(parents=True, exist_ok=True)

    model_config_path = write_cpp_model_config(run.raw, run.output_dir, run.mode)
    if model_config_path is not None and not model_config_path.is_file():
        raise FileNotFoundError(f"missing C++ model config: {model_config_path}")

    execute_trace_graph(build_trace_graph_command(run, model_config_path))
    run_summary = load_json(run.output_dir / "run_summary.json")
    prediction = {"predicted_e2e_us": int(run_summary.get("simulated_e2e_us", 0))}
    write_json(run.output_dir / "prediction.json", prediction)

    if run.outputs.validation:
        if validation_artifact_writer is None:
            raise RuntimeError("validation outputs require a workflow validation artifact writer")
        validation_artifact_writer(run, prediction, run_summary)
    return prediction


def main(
    argv: list[str] | None = None,
    *,
    validation_artifact_writer: ValidationArtifactWriter | None = None,
) -> int:
    """Run the container CLI while keeping help available on the host."""

    effective_argv = sys.argv[1:] if argv is None else argv
    if any(argument in {"-h", "--help"} for argument in effective_argv):
        parse_args(effective_argv)
        return 0
    require_modeling_container()
    run_from_config(parse_args(effective_argv), validation_artifact_writer=validation_artifact_writer)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
