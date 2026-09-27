"""Serial, separately budgeted base acquisition using the normal profile entrypoint."""

from __future__ import annotations

import copy
from dataclasses import asdict
from pathlib import Path
import tempfile
import time
from typing import Any, TYPE_CHECKING

from ..common.commands import command_tokens, replace_command_option
from ..common.io import load_json, write_json
from ..common.naming import sanitize
from ..common.paths import ROOT_DIR, repo_relative_path, require_repo_path
from ..contracts.forced_token.bundle import resolve_forced_token_bundle_plan
from ..profiling.suite import expand_suite, experiment_identity, filter_suite_experiments
from ..workload_template.cli import parse_workload_command
from ..workload_template.expand import request_token_budget
from ..workload_template.schema import load_template
from .capture import pending_group_capture, profile_usage, recorded_capture, run_profile_attempt

if TYPE_CHECKING:
    from .group import GroupRequest


def base_attempts(raw: dict[str, Any]) -> list[dict[str, Any]]:
    path = require_repo_path(raw["output_dir"]) / "base_capture_ledger.json"
    return load_json(path)["attempts"] if path.exists() else []


def _workload(raw: dict[str, Any], suite: dict[str, Any], workload: str) -> dict[str, Any]:
    experiments = filter_suite_experiments(
        list(enumerate(expand_suite(suite), start=1)),
        set(),
        selected_servers={raw["base_config"]},
        selected_inputs={workload},
    )
    if len(experiments) != 1:
        raise ValueError("base acquisition requires one declared experiment per base/workload")

    index, experiment = experiments[0]
    selected = copy.deepcopy(experiment)
    selected.pop("run_id", None)
    # Keep suite accounting and forced-token bundle output for this single run,
    # without reconstructing matrix axes and losing experiment-level overrides.
    selected["experiments"] = [{"id": experiment_identity(selected, index)}]
    selected.update(continue_on_error=False, name=f"{raw['base_config']}_{workload}_base")
    selected["server"]["startup_max_attempts"] = 1
    selected.setdefault("metadata", {}).update(
        purpose="Group base acquisition, separate from fixed calibration.",
        execution_contract="One declared base workload; serial and separately budgeted.",
    )
    argv = command_tokens(selected["bench"]["command"])
    selected["bench"]["command"] = argv
    args = parse_workload_command(argv)
    if args is None or args.forced_token_mode != "replay" or not args.forced_token_plan:
        raise ValueError("base acquisition requires a template forced-token replay experiment")
    direct_replay = args.forced_token_plan != "{forced_token_plan}"
    if direct_replay and raw.get("forced_token_bundle"):
        raise ValueError("base replay must use either an explicit token plan or a bundle, not both")
    template = load_template(require_repo_path(args.template))
    config = require_repo_path(args.config_specs)
    return {
        "workload": workload,
        "direct_replay": direct_replay,
        "counts": request_token_budget(template),
        "inputs": {
            "suite": selected,
            "template": dict(template.data),
            "config_specs": load_json(config),
            "forced_token_bundle": raw.get("forced_token_bundle"),
        },
    }


def captured_base_manifests(raw: dict[str, Any], suite: dict[str, Any], missing: set[str]) -> list[Path]:
    """Admit only completed current-input base runs, never discover a target matrix."""
    attempts = base_attempts(raw)
    result = []
    for workload in sorted(missing):
        rows = [
            row
            for row in attempts
            if row["workload"] == workload and row["mode"] == "replay" and row["status"] == "completed"
        ]
        if not rows:
            continue

        inputs = _workload(raw, suite, workload)["inputs"]
        row = next((row for row in reversed(rows) if row["inputs"] == inputs), None)
        if row:
            result.append(require_repo_path(row["profile_manifest"]))
    return result


def capture_base(group: GroupRequest, *, dry_run: bool) -> dict[str, Any]:
    """Capture missing base inputs; never borrow the fixed-calibration budget."""
    suite = load_json(group.profile_suite)
    jobs = [_workload(group.raw, suite, workload) for workload in group.missing_base_workloads]
    attempts = base_attempts(group.raw)
    budget = group.base_budget
    plan = {
        "status": "needs_base_capture",
        "base_capture_usage": profile_usage(attempts),
        "missing_base_workloads": list(group.missing_base_workloads),
        "base_capture_budget": asdict(budget) if budget else None,
        "base_jobs": [],
    }
    for job in jobs:
        previous = [row for row in attempts if row["workload"] == job["workload"] and row["inputs"] == job["inputs"]]
        captured = next(
            (row for row in reversed(previous) if row["mode"] == "capture" and row["status"] == "completed"), None
        )
        job["bundle"] = group.raw.get("forced_token_bundle") or (captured["forced_token_bundle"] if captured else None)
        job["modes"] = ["replay"] if job["bundle"] or job["direct_replay"] else ["capture", "replay"]
        plan["base_jobs"].append(
            {
                "workload": job["workload"],
                "modes": job["modes"],
                "requests": len(job["modes"]) * job["counts"]["requests"],
                "tokens": len(job["modes"]) * job["counts"]["tokens"],
            }
        )
    if dry_run or budget is None:
        plan["stop_reason"] = "dry_run" if dry_run else "base_budget_required"
        return plan
    if reason := pending_group_capture(group.output_dir):
        plan.update(status="capture_incomplete", stop_reason=reason)
        return plan
    ledger_path = group.output_dir / "base_capture_ledger.json"
    for job in jobs:
        for mode in job["modes"]:
            usage = profile_usage(attempts)
            counts = job["counts"]
            remaining, limits = budget.available_for(
                usage, server_starts=1, requests=counts["requests"], tokens=counts["tokens"]
            )
            if limits:
                plan.update(status="base_budget_exhausted", limits=limits)
                return plan
            work_root = group.output_dir / "base_captures"
            work_root.mkdir(parents=True, exist_ok=True)
            output = Path(tempfile.mkdtemp(prefix=f"{mode}_", dir=work_root))
            config = copy.deepcopy(job["inputs"]["suite"])
            argv = config["bench"]["command"]
            for flag, field in (("--template", "template"), ("--config-specs", "config_specs")):
                path = output / f"{field}.json"
                write_json(path, job["inputs"][field])
                replace_command_option(argv, flag, str(repo_relative_path(path)))
            if mode == "capture":
                args = parse_workload_command(argv)
                replace_command_option(argv, "--forced-token-mode", "capture")
                replace_command_option(
                    argv, "--forced-token-plan", str(Path(args.output_dir) / "forced_token_plan.json")
                )
                config["profiling"] = {"enabled": False, "channels": []}
                config["run_root"] = "data/no_profile_runs/sglang"
                config["metadata"]["profile_mode"] = "forced_token_capture"
                config["metadata"]["full_dag_contract"] = (
                    "Not applicable to token capture; the subsequent replay produces source DAG traces."
                )
            config["run_id"] = sanitize(f"{time.strftime('%Y%m%d_%H%M%S')}_base_{output.name}")
            suite_path = output / "suite.json"
            write_json(suite_path, config)
            command = [str(ROOT_DIR / "scripts/profile.sh"), str(repo_relative_path(suite_path))]
            row = {
                "workload": job["workload"],
                "inputs": job["inputs"],
                "mode": mode,
                "suite_dir": str(repo_relative_path(require_repo_path(config["run_root"]) / config["run_id"])),
                "suite_config": str(repo_relative_path(suite_path)),
                "command_log": str(repo_relative_path(output / "command.log")),
                "reserved_requests": counts["requests"],
                "reserved_tokens": counts["tokens"],
                "budgeted_output_tokens_per_request": counts["output_tokens_per_request"],
            }
            if mode == "replay" and job["bundle"]:
                resolve_forced_token_bundle_plan(require_repo_path(job["bundle"]), job["workload"])
                row["forced_token_bundle"] = job["bundle"]
                command.extend(("--forced-token-bundle", job["bundle"]))
            try:
                with recorded_capture(ledger_path, {"attempts": attempts}, row, profile_usage, kind="base"):
                    print(
                        f"base {job['workload']} {mode}: starting; remaining wall budget {remaining:.1f}s", flush=True
                    )
                    run_profile_attempt(row, command, remaining)
            finally:
                plan["base_capture_usage"] = profile_usage(attempts)
            if row["status"] != "completed":
                plan.update(status=row["status"], failed_workload=job["workload"], attempt=row)
                return plan
            if mode == "capture":
                job["bundle"] = row["forced_token_bundle"]
    plan["status"] = "base_captured"
    return plan
