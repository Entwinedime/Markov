#!/usr/bin/env python3
"""SGLang profiling runner。

本脚本只负责启动被测进程、注入采集环境、运行 workload，并写出 profile manifest。
建模判断不放在这里，避免 profiling 阶段和 modeling 阶段互相污染。
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path
from typing import Any


from ..common.io import load_json, write_json as dump_json
from ..common.logging import log
from ..common.naming import sanitize
from ..common.paths import ROOT_DIR, resolve_repo_path
from .executor import preflight_profile_config, run_profile
from ..contracts.forced_token import forced_token_bundle_summary
from .forced_workflow import (
    build_forced_token_bundle,
    inject_forced_token_bundle_plan,
)
from .suite import (
    PROFILE_EXPERIMENTS_ENV,
    PROFILE_FORCED_TOKEN_BUNDLE_ENV,
    PROFILE_INPUTS_ENV,
    PROFILE_SERVERS_ENV,
    describe_suite_experiment,
    experiment_identity,
    expand_suite,
    filter_suite_experiments,
    parse_experiment_selection,
    suite_profile_mode,
    summarize_suite_forced_token_contracts,
)

def run_profile_suite(
    cfg: dict[str, Any],
    dry_run: bool,
    selected_experiments: set[str] | None = None,
    selected_inputs: set[str] | None = None,
    selected_servers: set[str] | None = None,
    *,
    forced_token_bundle: Path | None = None,
    config_path: Path | None = None,
) -> list[Path]:
    """执行普通 run 或 suite，并写出 suite 级选择/结果文件。"""

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
    run_dirs: list[Path] = []
    failures: list[dict[str, Any]] = []
    continue_on_error = bool(cfg.get("continue_on_error", False))
    prepared_experiments: list[tuple[int, int, str, dict[str, Any], dict[str, Any]]] = []
    for ordinal, (index, experiment) in enumerate(experiments, start=1):
        exp_name = sanitize(str(experiment.get("name", f"experiment-{index}")))
        exp_cfg = inject_forced_token_bundle_plan(experiment, forced_token_bundle)
        exp_cfg["run_root"] = str(suite_dir)
        exp_cfg["run_id"] = f"{index:02d}_{exp_name}"
        forced_token_contract = preflight_profile_config(exp_cfg)
        prepared_experiments.append((ordinal, index, exp_name, exp_cfg, forced_token_contract))

    suite_dir.mkdir(parents=True, exist_ok=True)
    dump_json(suite_dir / "suite_config.json", cfg)
    log(f"Suite dir: {suite_dir}")
    dump_json(
        suite_dir / "suite_selection.json",
        {
            "schema": "trace_sim.profile.suite_selection.v1",
            "suite_name": suite_name,
            "framework": framework,
            "profile_mode": suite_profile_mode(cfg),
            "metadata": cfg.get("metadata", {}) if isinstance(cfg.get("metadata"), dict) else {},
            "selected_selectors": sorted(selected_experiments or []),
            "selected_inputs": sorted(selected_inputs or []),
            "selected_servers": sorted(selected_servers or []),
            "forced_token_bundle": forced_token_bundle_summary(forced_token_bundle)
            if forced_token_bundle is not None
            else None,
            "available_experiments": [describe_suite_experiment(index, experiment) for index, experiment in all_experiments],
            "planned_experiments": [
                {
                    **describe_suite_experiment(index, exp_cfg),
                    "forced_token_contract": forced_token_contract,
                }
                for _ordinal, index, _exp_name, exp_cfg, forced_token_contract in prepared_experiments
            ],
        },
    )

    fatal_error: Exception | None = None
    attempted_count = 0
    for ordinal, index, exp_name, exp_cfg, forced_token_contract in prepared_experiments:
        attempted_count += 1
        log(f"Suite experiment {ordinal}/{len(experiments)} (#{index}): {exp_name}")
        try:
            run_dirs.append(run_profile(exp_cfg, dry_run))
        except Exception as exc:
            failures.append({"name": exp_name, "error": str(exc), "forced_token_contract": forced_token_contract})
            if not continue_on_error:
                fatal_error = exc
                break

    generated_bundle: dict[str, Any] | None = None
    bundle_error: str | None = None
    if suite_profile_mode(cfg) == "forced_token_capture" and not dry_run and not failures:
        try:
            if config_path is None:
                raise ValueError("capture suite requires its source config path")
            generated_bundle = build_forced_token_bundle(
                suite_dir,
                run_dirs,
                capture_config_path=config_path,
            )
            log(f"Forced token bundle: {generated_bundle.get('path')}")
        except Exception as exc:
            bundle_error = str(exc)
            failures.append({"name": "forced_token_bundle", "error": bundle_error})

    dump_json(
        suite_dir / "suite_result.json",
        {
            "schema": "trace_sim.profile.suite_result.v1",
            "suite_dir": str(suite_dir),
            "suite_name": suite_name,
            "framework": framework,
            "profile_mode": suite_profile_mode(cfg),
            "metadata": cfg.get("metadata", {}) if isinstance(cfg.get("metadata"), dict) else {},
            "dry_run": bool(dry_run),
            "selected_selectors": sorted(selected_experiments or []),
            "selected_inputs": sorted(selected_inputs or []),
            "selected_servers": sorted(selected_servers or []),
            "planned_count": len(prepared_experiments),
            "attempted_count": attempted_count,
            "completed_count": len(run_dirs),
            "failure_count": len(failures),
            "aborted_count": len(prepared_experiments) - attempted_count,
            "status": "failed" if failures else "completed",
            "runs": [str(path) for path in run_dirs],
            "failures": failures,
            "forced_token_contracts": summarize_suite_forced_token_contracts(
                [
                    forced_token_contract
                    for _ordinal, _index, _exp_name, _exp_cfg, forced_token_contract in prepared_experiments
                ]
            ),
            "forced_token_bundle": forced_token_bundle_summary(forced_token_bundle)
            if forced_token_bundle is not None
            else None,
            "generated_forced_token_bundle": generated_bundle,
            "selected_experiments": [
                {
                    **describe_suite_experiment(index, exp_cfg),
                    "forced_token_contract": forced_token_contract,
                }
                for _ordinal, index, _exp_name, exp_cfg, forced_token_contract in prepared_experiments
            ],
        },
    )
    if bundle_error:
        raise ValueError(f"forced token bundle aggregation failed: {bundle_error}")
    if fatal_error is not None:
        raise RuntimeError(f"profile suite failed: {failures[-1]['name']}: {fatal_error}") from fatal_error
    return run_dirs


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """解析容器内 runner CLI 参数。"""

    parser = argparse.ArgumentParser(description="Run SGLang profiling experiments.")
    parser.add_argument("--config", required=True, help="JSON profile config path")
    parser.add_argument("--dry-run", action="store_true", help="expand config and manifest without starting the server")
    parser.add_argument("--experiment", action="append", default=[], help="run one experiment id/name; may be repeated")
    parser.add_argument("--experiments", action="append", default=[], help="comma-separated experiment ids/names to run")
    parser.add_argument("--input", action="append", default=[], help="run one suite input id; may be repeated")
    parser.add_argument("--inputs", action="append", default=[], help="comma-separated suite input ids to run")
    parser.add_argument("--server", action="append", default=[], help="run one suite server id; may be repeated")
    parser.add_argument("--servers", action="append", default=[], help="comma-separated suite server ids to run")
    parser.add_argument(
        "--forced-token-bundle",
        help="Explicit forced_token_bundle.json required by forced-token replay suites.",
    )
    parser.add_argument("--list-experiments", action="store_true", help="print expanded experiment ids without running them")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """CLI 入口：执行 profiling run/suite 或列出展开后的实验。"""

    args = parse_args(argv)
    config_path = resolve_repo_path(args.config)
    if config_path is None or not config_path.is_file():
        raise FileNotFoundError(f"missing config: {args.config}")
    cfg = load_json(config_path)
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
    forced_token_bundle = resolve_repo_path(
        args.forced_token_bundle or os.environ.get(PROFILE_FORCED_TOKEN_BUNDLE_ENV)
    )
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
        config_path=config_path,
    )
    for run_dir in run_dirs:
        print(run_dir)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(2)
    except KeyboardInterrupt:
        raise SystemExit(130)
