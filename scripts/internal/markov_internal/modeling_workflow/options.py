"""Conversion from lazily parsed CLI arguments to workflow contracts."""

from __future__ import annotations

import argparse
from pathlib import Path

from ..common.paths import ROOT_DIR, require_repo_path
from .context import WorkflowOptions
from .validations.registry import validation_names


PREDICTION_SCOPE_CHOICES = {"self", "cross"}


def workflow_options_from_args(args: argparse.Namespace) -> WorkflowOptions:
    """Normalize parsed values and resolve all paths under the repository."""

    validations = parse_required_csv(args.validations, set(validation_names()), label="validations")
    prediction_scope = parse_required_csv(
        args.prediction_scope,
        PREDICTION_SCOPE_CHOICES,
        label="prediction scope",
    )
    return WorkflowOptions(
        profile_run_dirs=tuple(require_repo_path(path) for path in args.profile_run_dir),
        manifests=tuple(require_repo_path(path) for path in args.manifest),
        output_dir=workflow_output_dir(args),
        validations=validations,
        input_ids=parse_optional_csv(args.inputs),
        config_ids=parse_optional_csv(args.configs),
        source_config_ids=parse_optional_csv(args.source_configs),
        target_config_ids=parse_optional_csv(args.target_configs),
        prediction_scope=frozenset(prediction_scope),
        dry_run=args.dry_run,
        force=args.force,
        continue_on_error=args.continue_on_error,
        max_predictions=args.max_predictions,
        page_key_mode=args.page_key_mode,
        sample_limit=args.sample,
        trace_threads=args.trace_threads,
        trace_file_threads=args.trace_file_threads,
        model_run_jobs=args.model_run_jobs,
    )


def parse_required_csv(raw: str, choices: set[str], *, label: str) -> tuple[str, ...]:
    """Parse a required comma-separated choice list while preserving order."""

    values = tuple(item.strip() for item in raw.split(",") if item.strip())
    if not values:
        raise SystemExit(f"No {label} selected.")
    unknown = sorted(set(values) - choices)
    if unknown:
        raise SystemExit(f"Unknown {label}: {', '.join(unknown)}")
    return values


def parse_optional_csv(raw: str) -> frozenset[str]:
    """Parse an optional comma-separated selector into a membership set."""

    return frozenset(item.strip() for item in raw.split(",") if item.strip())


def workflow_output_dir(args: argparse.Namespace) -> Path:
    """Resolve the explicit output directory or the stable default location."""

    if args.output_dir:
        return require_repo_path(args.output_dir)
    if args.profile_run_dir:
        return require_repo_path(args.profile_run_dir[0]) / "modeling" / "modeling_workflow"
    return ROOT_DIR / "data/modeling_runs/modeling_workflow"


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
