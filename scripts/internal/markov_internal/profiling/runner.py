#!/usr/bin/env python3
"""Shared profiling runner for SGLang and KTransformers.

This module starts the profiled process, injects capture state, runs the
workload, and writes profile manifests. Modeling decisions are intentionally
excluded so capture facts cannot be contaminated by prediction policy.
"""

from __future__ import annotations

import argparse
import os
import time
from pathlib import Path
from typing import Any


from ..common.io import load_json, write_json as dump_json
from ..common.logging import log
from ..common.naming import sanitize
from ..common.paths import ROOT_DIR, repo_relative_path, require_repo_path, resolve_repo_path
from .executor import ProfileRun
from .frameworks import framework_adapter
from .forced_workflow import (
    build_forced_token_bundle,
    inject_forced_token_bundle_plan,
)
from .suite import (
    experiment_identity,
    expand_suite,
    filter_suite_experiments,
    parse_experiment_selection,
    narrow_profile_channels,
    suite_profile_mode,
)


def run_profile_suite(
    cfg: dict[str, Any],
    dry_run: bool,
    experiments: list[tuple[int, dict[str, Any]]],
    *,
    forced_token_bundle: Path | None = None,
) -> list[Path]:
    """Execute one run or suite and persist one concise result artifact."""

    is_suite = "experiments" in cfg or "matrix" in cfg
    if len(experiments) == 1 and not is_suite:
        experiment = inject_forced_token_bundle_plan(experiments[0][1], forced_token_bundle)
        run = ProfileRun(experiment, dry_run=dry_run)
        return [run.run()]

    framework = cfg.get("framework", "sglang")
    suite_name = sanitize(str(cfg.get("name", f"{framework}-profile-suite")))
    suite_root = resolve_repo_path(cfg.get("run_root")) or ROOT_DIR / "data/profile_runs" / str(framework)
    suite_id = cfg.get("run_id") or f"{time.strftime('%Y%m%d_%H%M%S')}_{suite_name}"
    suite_dir = suite_root / sanitize(str(suite_id))
    continue_on_error = bool(cfg.get("continue_on_error", False))
    prepared_experiments = _prepare_suite_experiments(experiments, suite_dir, forced_token_bundle, dry_run=dry_run)

    suite_dir.mkdir(parents=True, exist_ok=True)
    log(f"Suite dir: {suite_dir}")

    run_dirs: list[Path] = []
    failures: list[dict[str, Any]] = []
    attempted_count = 0
    generated_bundle = None
    finished = False
    summary = {
        "suite_dir": str(suite_dir),
        "suite_name": suite_name,
        "framework": framework,
        "profile_mode": suite_profile_mode(cfg),
        "dry_run": dry_run,
        "planned_count": len(prepared_experiments),
        "profile_manifests": [str(run.layout.run_dir / "profile_manifest.json") for _, run in prepared_experiments],
        "status": "running",
        "forced_token_bundle": {"path": str(forced_token_bundle)} if forced_token_bundle is not None else None,
    }
    # Declare output locations before inference, including runs that may fail or
    # be interrupted. Consumers must not discover another manifest by globbing.
    dump_json(suite_dir / "suite_result.json", summary)
    try:
        for ordinal, (index, run) in enumerate(prepared_experiments, start=1):
            name = sanitize(str(run.cfg.get("name", f"experiment-{index}")))
            log(f"Suite experiment {ordinal}/{len(prepared_experiments)} (#{index}): {name}")
            attempted_count += 1
            try:
                run_dirs.append(run.run())
            except BaseException as error:
                failures.append({"name": name, "error": str(error) or type(error).__name__})
                if not isinstance(error, Exception) or not continue_on_error:
                    raise
        if suite_profile_mode(cfg) == "forced_token_capture" and not dry_run and not failures:
            try:
                generated_bundle = build_forced_token_bundle(suite_dir, run_dirs)
                log(f"Forced token bundle: {generated_bundle['path']}")
            except BaseException as error:
                failures.append({"name": "forced_token_bundle", "error": str(error) or type(error).__name__})
                raise
        finished = True
    finally:
        dump_json(
            suite_dir / "suite_result.json",
            {
                **summary,
                "attempted_count": attempted_count,
                "completed_count": len(run_dirs),
                "failure_count": len(failures),
                "aborted_count": len(prepared_experiments) - attempted_count,
                "status": "completed" if finished and not failures else "failed",
                "runs": [str(path) for path in run_dirs],
                "failures": failures,
                "generated_forced_token_bundle": generated_bundle,
            },
        )
    if failures:
        raise RuntimeError(f"{len(failures)} profiling experiments failed; see {suite_dir / 'suite_result.json'}")
    return run_dirs


def _prepare_suite_experiments(
    experiments: list[tuple[int, dict[str, Any]]],
    suite_dir: Path,
    forced_token_bundle: Path | None,
    *,
    dry_run: bool,
) -> list[tuple[int, ProfileRun]]:
    """Inject run-local paths and validate every selected experiment upfront."""

    prepared = []
    for index, experiment in experiments:
        name = sanitize(str(experiment.get("name", f"experiment-{index}")))
        config = inject_forced_token_bundle_plan(experiment, forced_token_bundle)
        config["run_root"] = str(suite_dir)
        config["run_id"] = f"{index:02d}_{name}"
        prepared.append((index, ProfileRun(config, dry_run=dry_run)))
    return prepared


def parse_args(argv: list[str] | None = None, *, host: bool = False) -> argparse.Namespace:
    """Share selection options; only the host uses a positional config path."""

    parser = argparse.ArgumentParser(
        prog="scripts/profile.sh" if host else None,
        description="Run profiling experiments in the framework selected by the JSON config.",
        allow_abbrev=False,
    )
    if host:
        parser.add_argument("config", help="JSON profile config path inside the repository")
    else:
        parser.add_argument("--config", required=True, help="JSON profile config path")
    parser.add_argument("--dry-run", action="store_true", help="expand config and manifest without starting the server")
    parser.add_argument(
        "--experiment",
        "--experiments",
        dest="experiments",
        action="append",
        default=[],
        help="Experiment ids/names, comma-separated or repeated.",
    )
    parser.add_argument(
        "--input",
        "--inputs",
        dest="inputs",
        action="append",
        default=[],
        help="Suite input ids, comma-separated or repeated.",
    )
    parser.add_argument(
        "--server",
        "--servers",
        dest="servers",
        action="append",
        default=[],
        help="Suite server ids, comma-separated or repeated.",
    )
    parser.add_argument(
        "--forced-token-bundle",
        help="Resolve {forced_token_plan} in replay commands from a workload-to-plan bundle.",
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


def host_main(argv: list[str] | None = None) -> None:
    """Replace the host launcher with Docker; no server or profile runs on the host."""

    args = parse_args(argv, host=True)
    config = require_repo_path(args.config).resolve()
    config_relative = repo_relative_path(config)
    framework = framework_adapter(load_json(config).get("framework", "sglang"))
    container_root = Path("/workspace/trace-sim")
    runner_args = ["--config", str(container_root / config_relative)]
    for option in ("experiments", "inputs", "servers", "channels"):
        runner_args.extend(f"--{option}={value}" for value in getattr(args, option))
    for option in ("dry_run", "list_experiments"):
        if getattr(args, option):
            runner_args.append("--" + option.replace("_", "-"))
    if args.forced_token_bundle:
        bundle = require_repo_path(args.forced_token_bundle).resolve()
        bundle_relative = repo_relative_path(bundle)
        if not bundle.is_file():
            raise FileNotFoundError(f"forced token bundle does not exist: {bundle}")
        runner_args.extend(("--forced-token-bundle", str(container_root / bundle_relative)))

    command = ["docker", "compose", "-f", "docker/compose/inference.yml", "run", "--rm"]
    name = os.environ.get("TRACE_SIM_PROFILE_CONTAINER_NAME")
    if name:
        command.extend(("--name", name))
    container_command = """
set -euo pipefail
set +u
source /usr/local/Ascend/ascend-toolkit/set_env.sh
set -u
export LD_LIBRARY_PATH="/usr/local/Ascend/driver/lib64/common:/usr/local/Ascend/driver/lib64/driver:${LD_LIBRARY_PATH:-}"
export HOOK_ASCENDCL_SO_PATH="/usr/local/Ascend/ascend-toolkit/latest/lib64/libascendcl.so"
export PYTHONPATH="scripts/internal${PYTHONPATH:+:$PYTHONPATH}"
exec python3 -m markov_internal.profiling.runner "$@"
"""
    command.extend((framework.name + "-profile", "bash", "-lc", container_command, "bash", *runner_args))
    os.chdir(ROOT_DIR)
    os.execvp(command[0], command)


def main(argv: list[str] | None = None) -> int:
    """Execute profiling or list the selected expanded experiments."""

    args = parse_args(argv)
    config_path = resolve_repo_path(args.config)
    if config_path is None or not config_path.is_file():
        raise FileNotFoundError(f"missing config: {args.config}")
    cfg = load_json(config_path)
    selected_channels = parse_experiment_selection(args.channels)
    if selected_channels:
        cfg = narrow_profile_channels(cfg, selected_channels)
    forced_token_bundle = resolve_repo_path(args.forced_token_bundle)
    experiments = filter_suite_experiments(
        list(enumerate(expand_suite(cfg), start=1)),
        parse_experiment_selection(args.experiments),
        selected_inputs=parse_experiment_selection(args.inputs),
        selected_servers=parse_experiment_selection(args.servers),
    )
    if args.list_experiments:
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
        experiments,
        forced_token_bundle=forced_token_bundle,
    )
    for run_dir in run_dirs:
        print(run_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
