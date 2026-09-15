"""Conversion from lazily parsed CLI arguments to workflow contracts."""

from __future__ import annotations

import argparse
from pathlib import Path

from ..common.paths import require_repo_path
from .artifacts import ArtifactPolicy
from .context import WorkflowOptions
from .io_model import HiCacheIoModel
from .planning.target_configs import load_target_configs


def workflow_options_from_args(args: argparse.Namespace) -> WorkflowOptions:
    """Normalize parsed values and resolve all paths under the repository."""

    artifact_policy = ArtifactPolicy.from_value(args.diagnostics)
    return WorkflowOptions(
        source_manifests=tuple(require_repo_path(path) for path in args.source_manifest),
        target_configs=load_target_configs(args.target_config),
        output_dir=workflow_output_dir(args),
        artifact_policy=artifact_policy,
        dry_run=args.dry_run,
        continue_on_error=args.continue_on_error,
        max_predictions=args.max_predictions,
        trace_threads=args.trace_threads,
        trace_file_threads=args.trace_file_threads,
        model_run_jobs=args.model_run_jobs,
        hicache_io_model=HiCacheIoModel.load(args.hicache_io_model) if args.hicache_io_model else None,
    )


def workflow_output_dir(args: argparse.Namespace) -> Path:
    """Resolve the explicit output directory or the stable default location."""

    if args.output_dir:
        return require_repo_path(args.output_dir)
    return require_repo_path(args.source_manifest[0]).parent / "modeling" / "hicache_prediction"


def positive_int(raw: str) -> int:
    """Parse a strictly positive integer for ``argparse``."""

    value = int(raw)
    if value < 1:
        raise argparse.ArgumentTypeError("expected a positive integer")
    return value


def nonnegative_int(raw: str) -> int:
    """Parse a non-negative integer for ``argparse``."""

    value = int(raw)
    if value < 0:
        raise argparse.ArgumentTypeError("expected a non-negative integer")
    return value
