#!/usr/bin/env python3
"""Shared profiling runner for SGLang and KTransformers.

This module starts the profiled process, injects capture state, runs the
workload, and writes profile manifests. Modeling decisions are intentionally
excluded so capture facts cannot be contaminated by prediction policy.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


from ..common.io import load_json, write_json as dump_json
from ..common.logging import log
from ..common.naming import sanitize
from ..common.paths import ROOT_DIR, resolve_repo_path
from .executor import preflight_profile_config, run_profile
from ..contracts.forced_token.bundle import forced_token_bundle_summary
from .forced_workflow import (
    build_forced_token_bundle,
    inject_forced_token_bundle_plan,
)
from .suite import (
    PROFILE_EXPERIMENTS_ENV,
    PROFILE_CHANNELS_ENV,
    PROFILE_FORCED_TOKEN_BUNDLE_ENV,
    PROFILE_INPUTS_ENV,
    PROFILE_SERVERS_ENV,
    experiment_identity,
    expand_suite,
    filter_suite_experiments,
    parse_experiment_selection,
    narrow_profile_channels,
    suite_profile_mode,
    summarize_suite_forced_token_contracts,
)


@dataclass(frozen=True)
class _PreparedExperiment:
    """One validated suite experiment with stable display and artifact metadata."""

    ordinal: int
    index: int
    name: str
    config: dict[str, Any]
    forced_token_contract: dict[str, Any]


@dataclass(frozen=True)
class _SuiteExecution:
    """Execution outcome retained until the aggregate suite artifact is written."""

    run_dirs: list[Path]
    failures: list[dict[str, Any]]
    attempted_count: int
    fatal_error: Exception | None


def run_profile_suite(
    cfg: dict[str, Any],
    dry_run: bool,
    selected_experiments: set[str] | None = None,
    selected_inputs: set[str] | None = None,
    selected_servers: set[str] | None = None,
    *,
    forced_token_bundle: Path | None = None,
) -> list[Path]:
    """Execute one run or suite and persist one concise result artifact."""

    is_suite = "experiments" in cfg or "matrix" in cfg
    all_experiments = list(enumerate(expand_suite(cfg), start=1))
    experiments = filter_suite_experiments(
        all_experiments,
        selected_experiments or set(),
        selected_inputs=selected_inputs,
        selected_servers=selected_servers,
    )
    if len(experiments) == 1 and not is_suite:
        experiment = inject_forced_token_bundle_plan(experiments[0][1], forced_token_bundle)
        preflight_profile_config(experiment)
        return [run_profile(experiment, dry_run)]

    framework = cfg.get("framework", "sglang")
    suite_name = sanitize(str(cfg.get("name", f"{framework}-profile-suite")))
    suite_root = resolve_repo_path(cfg.get("run_root")) or ROOT_DIR / "data/profile_runs" / str(framework)
    suite_id = cfg.get("run_id") or f"{time.strftime('%Y%m%d_%H%M%S')}_{suite_name}"
    suite_dir = suite_root / sanitize(str(suite_id))
    continue_on_error = bool(cfg.get("continue_on_error", False))
    prepared_experiments = _prepare_suite_experiments(experiments, suite_dir, forced_token_bundle)

    suite_dir.mkdir(parents=True, exist_ok=True)
    log(f"Suite dir: {suite_dir}")

    execution = _execute_suite_experiments(
        prepared_experiments,
        dry_run=dry_run,
        continue_on_error=continue_on_error,
    )
    generated_bundle, bundle_error = _build_capture_bundle_if_requested(
        cfg,
        dry_run=dry_run,
        suite_dir=suite_dir,
        run_dirs=execution.run_dirs,
        failures=execution.failures,
    )
    if bundle_error:
        execution.failures.append({"name": "forced_token_bundle", "error": bundle_error})

    dump_json(
        suite_dir / "suite_result.json",
        {
            "suite_dir": str(suite_dir),
            "suite_name": suite_name,
            "framework": framework,
            "profile_mode": suite_profile_mode(cfg),
            "dry_run": bool(dry_run),
            "planned_count": len(prepared_experiments),
            "attempted_count": execution.attempted_count,
            "completed_count": len(execution.run_dirs),
            "failure_count": len(execution.failures),
            "aborted_count": len(prepared_experiments) - execution.attempted_count,
            "status": "failed" if execution.failures else "completed",
            "runs": [str(path) for path in execution.run_dirs],
            "failures": execution.failures,
            "forced_token_contracts": summarize_suite_forced_token_contracts(
                [experiment.forced_token_contract for experiment in prepared_experiments]
            ),
            "forced_token_bundle": forced_token_bundle_summary(forced_token_bundle)
            if forced_token_bundle is not None
            else None,
            "generated_forced_token_bundle": generated_bundle,
        },
    )
    if bundle_error:
        raise ValueError(f"forced token bundle aggregation failed: {bundle_error}")
    if execution.fatal_error is not None:
        raise RuntimeError(
            f"profile suite failed: {execution.failures[-1]['name']}: {execution.fatal_error}"
        ) from execution.fatal_error
    return execution.run_dirs


def _prepare_suite_experiments(
    experiments: list[tuple[int, dict[str, Any]]],
    suite_dir: Path,
    forced_token_bundle: Path | None,
) -> list[_PreparedExperiment]:
    """Inject run-local paths and validate every selected experiment upfront."""

    prepared: list[_PreparedExperiment] = []
    for ordinal, (index, experiment) in enumerate(experiments, start=1):
        name = sanitize(str(experiment.get("name", f"experiment-{index}")))
        config = inject_forced_token_bundle_plan(experiment, forced_token_bundle)
        config["run_root"] = str(suite_dir)
        config["run_id"] = f"{index:02d}_{name}"
        prepared.append(
            _PreparedExperiment(
                ordinal=ordinal,
                index=index,
                name=name,
                config=config,
                forced_token_contract=preflight_profile_config(config),
            )
        )
    return prepared


def _execute_suite_experiments(
    experiments: list[_PreparedExperiment],
    *,
    dry_run: bool,
    continue_on_error: bool,
) -> _SuiteExecution:
    """Run prepared experiments sequentially under the suite failure policy."""

    run_dirs: list[Path] = []
    failures: list[dict[str, Any]] = []
    fatal_error: Exception | None = None
    attempted_count = 0
    for experiment in experiments:
        attempted_count += 1
        log(f"Suite experiment {experiment.ordinal}/{len(experiments)} (#{experiment.index}): {experiment.name}")
        try:
            run_dirs.append(run_profile(experiment.config, dry_run))
        except Exception as error:
            failures.append(
                {
                    "name": experiment.name,
                    "error": str(error),
                }
            )
            if not continue_on_error:
                fatal_error = error
                break
    return _SuiteExecution(run_dirs, failures, attempted_count, fatal_error)


def _build_capture_bundle_if_requested(
    cfg: dict[str, Any],
    *,
    dry_run: bool,
    suite_dir: Path,
    run_dirs: list[Path],
    failures: list[dict[str, Any]],
) -> tuple[dict[str, Any] | None, str | None]:
    """Build a capture bundle only after every selected experiment succeeds."""

    if suite_profile_mode(cfg) != "forced_token_capture" or dry_run or failures:
        return None, None
    try:
        bundle = build_forced_token_bundle(suite_dir, run_dirs)
        log(f"Forced token bundle: {bundle.get('path')}")
        return bundle, None
    except Exception as error:
        return None, str(error)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse the container-side profiling runner CLI."""

    parser = argparse.ArgumentParser(description="Run profiling experiments.")
    parser.add_argument("--config", required=True, help="JSON profile config path")
    parser.add_argument("--dry-run", action="store_true", help="expand config and manifest without starting the server")
    parser.add_argument("--experiment", action="append", default=[], help="run one experiment id/name; may be repeated")
    parser.add_argument(
        "--experiments", action="append", default=[], help="comma-separated experiment ids/names to run"
    )
    parser.add_argument("--input", action="append", default=[], help="run one suite input id; may be repeated")
    parser.add_argument("--inputs", action="append", default=[], help="comma-separated suite input ids to run")
    parser.add_argument("--server", action="append", default=[], help="run one suite server id; may be repeated")
    parser.add_argument("--servers", action="append", default=[], help="comma-separated suite server ids to run")
    parser.add_argument(
        "--forced-token-bundle",
        help="Explicit forced_token_bundle.json required by forced-token replay suites.",
    )
    parser.add_argument(
        "--channels",
        action="append",
        default=[],
        help="Narrow a suite to a configured channel subset for a clearly labeled diagnostic run.",
    )
    parser.add_argument(
        "--list-experiments", action="store_true", help="print expanded experiment ids without running them"
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """Execute profiling or list the selected expanded experiments."""

    args = parse_args(argv)
    config_path = resolve_repo_path(args.config)
    if config_path is None or not config_path.is_file():
        raise FileNotFoundError(f"missing config: {args.config}")
    cfg = load_json(config_path)
    selected_channels = parse_experiment_selection(
        args.channels,
        os.environ.get(PROFILE_CHANNELS_ENV),
    )
    if selected_channels:
        cfg = narrow_profile_channels(cfg, selected_channels)
    selected_experiments = parse_experiment_selection(
        [*args.experiment, *args.experiments],
        os.environ.get(PROFILE_EXPERIMENTS_ENV),
    )
    selected_inputs = parse_experiment_selection(
        [*args.input, *args.inputs],
        os.environ.get(PROFILE_INPUTS_ENV),
    )
    selected_servers = parse_experiment_selection(
        [*args.server, *args.servers],
        os.environ.get(PROFILE_SERVERS_ENV),
    )
    forced_token_bundle = resolve_repo_path(args.forced_token_bundle or os.environ.get(PROFILE_FORCED_TOKEN_BUNDLE_ENV))
    if args.list_experiments:
        experiments = filter_suite_experiments(
            list(enumerate(expand_suite(cfg), start=1)),
            selected_experiments,
            selected_inputs=selected_inputs,
            selected_servers=selected_servers,
        )
        for index, experiment in experiments:
            exp_id = experiment.get("id") or experiment_identity(experiment, index)
            exp_name = experiment.get("name") or exp_id
            metadata = experiment.get("metadata") if isinstance(experiment.get("metadata"), dict) else {}
            print(
                f"{index:02d}\t{exp_id}\t{exp_name}\t"
                f"server={metadata.get('suite_server_id')}\tinput={metadata.get('suite_input_id')}"
            )
        return 0

    run_dirs = run_profile_suite(
        cfg,
        args.dry_run,
        selected_experiments,
        selected_inputs,
        selected_servers,
        forced_token_bundle=forced_token_bundle,
    )
    for run_dir in run_dirs:
        print(run_dir)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(2) from exc
    except KeyboardInterrupt:
        raise SystemExit(130) from None
