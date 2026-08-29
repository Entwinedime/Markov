"""Conversion from lazily parsed CLI arguments to workflow contracts."""

from __future__ import annotations

import argparse
from pathlib import Path

from ..common.io import load_json
from ..common.paths import ROOT_DIR, require_repo_path
from .artifacts import ArtifactPolicy
from .context import WorkflowOptions
from .io_model import HiCacheIoModel
from .planning.target_configs import load_target_configs


def workflow_options_from_args(args: argparse.Namespace) -> WorkflowOptions:
    """Normalize parsed values and resolve all paths under the repository."""

    artifact_policy = ArtifactPolicy.from_value(args.diagnostics)
    return WorkflowOptions(
        profile_run_dirs=tuple(require_repo_path(path) for path in args.profile_run_dir),
        source_manifests=tuple(require_repo_path(path) for path in args.source_manifest),
        target_configs=load_target_configs(args.target_config),
        output_dir=workflow_output_dir(args),
        input_ids=parse_optional_csv(args.inputs),
        config_ids=parse_optional_csv(args.configs),
        source_config_ids=parse_optional_csv(args.source_configs),
        target_config_ids=parse_optional_csv(args.target_configs),
        artifact_policy=artifact_policy,
        dry_run=args.dry_run,
        continue_on_error=args.continue_on_error,
        max_predictions=args.max_predictions,
        trace_threads=args.trace_threads,
        trace_file_threads=args.trace_file_threads,
        model_run_jobs=args.model_run_jobs,
        hicache_io_model=HiCacheIoModel.load(args.hicache_io_model) if args.hicache_io_model else None,
        base_io_models=parse_base_io_models(args.base_io_model),
        oracle_scores=load_oracle_scores(args.oracle_scores),
        evaluation=args.evaluation,
    )


def parse_optional_csv(raw: str) -> frozenset[str]:
    """Parse an optional comma-separated selector into a membership set."""

    return frozenset(item.strip() for item in raw.split(",") if item.strip())


def parse_base_io_models(values: list[str]) -> tuple[tuple[str, HiCacheIoModel], ...]:
    """Parse repeatable evaluation-only ``CONFIG=PATH`` model bindings."""

    models: dict[str, HiCacheIoModel] = {}
    for value in values:
        config_id, separator, path = value.partition("=")
        if not separator or not config_id.strip() or not path.strip():
            raise SystemExit("--base-io-model expects CONFIG=PATH")
        config_id = config_id.strip()
        if config_id in models:
            raise SystemExit(f"duplicate --base-io-model config: {config_id}")
        models[config_id] = HiCacheIoModel.load(Path(path.strip()))
    return tuple(sorted(models.items()))


def load_oracle_scores(path: Path | None) -> tuple[tuple[str, Path, Path], ...]:
    if path is None:
        return ()
    payload = load_json(require_repo_path(path))
    rows = payload.get("bases") if isinstance(payload, dict) else None
    if not isinstance(rows, list) or not rows:
        raise ValueError("oracle score manifest requires bases")
    result = tuple(
        (
            str(row["config_id"]),
            require_repo_path(row["base_observations"]),
            require_repo_path(row["target_costs"]),
        )
        for row in rows
    )
    if len({config for config, _, _ in result}) != len(result):
        raise ValueError("oracle score manifest config_id values must be unique")
    return result


def workflow_output_dir(args: argparse.Namespace) -> Path:
    """Resolve the explicit output directory or the stable default location."""

    if args.output_dir:
        return require_repo_path(args.output_dir)
    if args.source_manifest:
        return require_repo_path(args.source_manifest[0]).parent / "modeling" / "hicache_prediction"
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
