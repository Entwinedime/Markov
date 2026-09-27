#!/usr/bin/env python3
"""Container-side orchestration for one C++ modeling run."""

from __future__ import annotations

import argparse
from pathlib import Path

from ..common.commands import positive_int
from ..common.paths import require_repo_path, running_in_modeling_container
from ..common.process import run_command
from .backend import build_trace_graph_command
from .run_config import ModelingOutputs, ModelingRunConfig
from .workload import discover_workload_window


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse generated replay or direct manifest DAG-build inputs."""

    parser = argparse.ArgumentParser(description="Build and simulate a source DAG, optionally applying a model.")
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--config", type=Path, help="self-contained modeling runner config")
    inputs.add_argument("--profile-manifest", type=Path, help="framework-neutral profile manifest")
    parser.add_argument("--output-dir", type=Path, help="required with --profile-manifest")
    parser.add_argument("--model-config", type=Path, help="optional DAG transforms with --profile-manifest")
    parser.add_argument("--threads", type=positive_int)
    parser.add_argument("--file-threads", type=positive_int)
    parser.add_argument("--emit-dag", action="store_true")
    args = parser.parse_args(argv)
    if args.config and (
        args.output_dir is not None
        or args.model_config is not None
        or args.threads is not None
        or args.file_threads is not None
        or args.emit_dag
    ):
        parser.error("--config replays its stored options; use --profile-manifest for direct DAG options")
    return args


def manifest_run_config(args: argparse.Namespace) -> ModelingRunConfig:
    """Prepare the framework-neutral DAG request for the common execution path."""
    if args.output_dir is None:
        raise SystemExit("--profile-manifest requires --output-dir")
    manifest, output = require_repo_path(args.profile_manifest), require_repo_path(args.output_dir)
    cpp_config = {"threads": args.threads or 1, "file_threads": args.file_threads or 1}
    window = discover_workload_window({}, manifest)
    if window is not None:
        cpp_config["trace_window_start_us"] = window.start_ns // 1000
        cpp_config["trace_window_end_us"] = window.end_ns // 1000
    return ModelingRunConfig(
        output,
        manifest,
        cpp_config,
        ModelingOutputs(dag_chrome_trace=args.emit_dag),
        model_config_path=require_repo_path(args.model_config) if args.model_config else None,
    )


def main(argv: list[str] | None = None) -> int:
    """Run C++ with inherited output and preserve its exit status.

    The workflow owns log retention and summary reading; this process only
    translates the request. Argument parsing keeps help available on the host.
    """

    args = parse_args(argv)
    if not running_in_modeling_container():
        raise SystemExit("use scripts/model.sh for containerized DAG modeling")

    if args.config:
        run = ModelingRunConfig.load(require_repo_path(args.config))
    else:
        run = manifest_run_config(args)

    run.output_dir.mkdir(parents=True, exist_ok=True)
    return run_command(build_trace_graph_command(run)).returncode


if __name__ == "__main__":
    raise SystemExit(main())
