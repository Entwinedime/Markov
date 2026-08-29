#!/usr/bin/env python3
"""Container-side orchestration for one C++ modeling run."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.paths import require_repo_path, running_in_modeling_container
from .backend import append_option, build_trace_graph_command, execute_trace_graph
from .cpp_config import cpp_model_config_path, trace_graph_executable
from .run_config import ModelingRunConfig


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse generated replay or direct manifest DAG-build inputs."""

    parser = argparse.ArgumentParser(description="Run one generated C++ modeling config.")
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--config", type=Path, help="self-contained modeling runner config")
    inputs.add_argument("--profile-manifest", type=Path, help="framework-neutral profile manifest")
    parser.add_argument("--output-dir", type=Path, help="required with --profile-manifest")
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--file-threads", type=int, default=1)
    parser.add_argument("--emit-dag", action="store_true")
    return parser.parse_args(argv)


def run_from_config(
    config_path: Path,
) -> dict[str, Any]:
    """Run C++ and return its canonical compact summary."""

    run = ModelingRunConfig.load(config_path)
    run.output_dir.mkdir(parents=True, exist_ok=True)

    model_config_path = cpp_model_config_path(run.raw, run.mode)
    if model_config_path is not None and not model_config_path.is_file():
        raise FileNotFoundError(f"missing C++ model config: {model_config_path}")

    execute_trace_graph(build_trace_graph_command(run, model_config_path))
    return load_json(run.output_dir / "run_summary.json")


def run_from_manifest(args: argparse.Namespace) -> dict[str, Any]:
    if args.output_dir is None:
        raise SystemExit("--profile-manifest requires --output-dir")
    manifest, output = require_repo_path(args.profile_manifest), require_repo_path(args.output_dir)
    output.mkdir(parents=True, exist_ok=True)
    command = [
        str(trace_graph_executable({})), "--profile-manifest", str(manifest),
        "--run-summary", str(output / "run_summary.json"),
    ]
    append_option(command, "--threads", args.threads)
    append_option(command, "--file-threads", args.file_threads)
    if args.emit_dag:
        command.extend(("--graph-output", str(output / "dag_chrome_trace.json")))
    execute_trace_graph(command)
    return load_json(output / "run_summary.json")


def main(
    argv: list[str] | None = None,
) -> int:
    """Run the container CLI while keeping help available on the host."""

    effective_argv = sys.argv[1:] if argv is None else argv
    if any(argument in {"-h", "--help"} for argument in effective_argv):
        parse_args(effective_argv)
        return 0
    if not running_in_modeling_container():
        raise SystemExit("use scripts/model.sh for containerized DAG modeling")
    args = parse_args(effective_argv)
    if args.config:
        run_from_config(require_repo_path(args.config))
    else:
        run_from_manifest(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
