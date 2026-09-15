"""Serial, separately budgeted base acquisition using the normal profile entrypoint."""

from __future__ import annotations

import copy
from dataclasses import asdict
import os
from pathlib import Path
import tempfile
import time
from typing import Any, TYPE_CHECKING

from ..common.commands import command_tokens
from ..common.io import load_json, write_json
from ..common.naming import sanitize
from ..common.paths import ROOT_DIR, repo_relative_path, require_repo_path
from ..contracts.forced_token.bundle import resolve_forced_token_bundle_plan
from ..profiling.suite import matrix_entries
from ..workload_template.expand import request_token_budget
from ..workload_template.schema import load_template
from .capture import has_pending_capture, profile_usage, run_profile_attempt

if TYPE_CHECKING:
    from .group import GroupRequest


def base_attempts(raw: dict[str, Any]) -> list[dict[str, Any]]:
    path = require_repo_path(raw["output_dir"]) / "base_capture_ledger.json"
    return load_json(path)["attempts"] if path.exists() else []


def _workload(raw: dict[str, Any], suite: dict[str, Any], workload: str) -> dict[str, Any]:
    selected = copy.deepcopy(suite)
    selected.pop("experiments", None)
    selected.pop("run_id", None)
    selected["matrix"] = {"servers": [copy.deepcopy(matrix_entries(suite["matrix"], "servers")[raw["base_config"]])],
                          "inputs": [copy.deepcopy(matrix_entries(suite["matrix"], "inputs")[workload])]}
    selected.update(continue_on_error=False, name=f"{raw['base_config']}_{workload}_base")
    selected["matrix"]["servers"][0]["server"]["startup_max_attempts"] = 1
    selected.setdefault("metadata", {}).update(purpose="Group base acquisition, separate from fixed calibration.",
                                               execution_contract="One declared base workload; serial and separately budgeted.")
    argv = command_tokens(selected["matrix"]["inputs"][0]["bench"]["command"])
    selected["matrix"]["inputs"][0]["bench"]["command"] = argv
    if "--template" not in argv or "--forced-token-mode" not in argv or "--forced-token-plan" not in argv:
        raise ValueError("base acquisition requires the normal template forced-token replay suite")
    template = require_repo_path(argv[argv.index("--template") + 1])
    config = require_repo_path(argv[argv.index("--config-specs") + 1])
    return {"workload": workload, "suite": selected, "counts": request_token_budget(load_template(template)),
            "inputs": {"suite": selected, "template": load_json(template), "config_specs": load_json(config),
                       "forced_token_bundle": raw.get("forced_token_bundle")}}


def matching_base_attempts(raw: dict[str, Any], suite: dict[str, Any], workload: str) -> list[dict[str, Any]]:
    attempts = [row for row in base_attempts(raw) if row["workload"] == workload]
    if not attempts:
        return []
    inputs = _workload(raw, suite, workload)["inputs"]
    return [row for row in attempts if row["inputs"] == inputs]


def captured_base_manifests(raw: dict[str, Any], suite: dict[str, Any], missing: set[str]) -> list[Path]:
    """Admit only completed current-input base runs, never discover a target matrix."""
    result = []
    for workload in sorted(missing):
        rows = matching_base_attempts(raw, suite, workload)
        row = next((row for row in reversed(rows) if row["mode"] == "replay" and row["status"] == "completed"), None)
        if row:
            result.append(require_repo_path(row["profile_manifest"]))
    return result


def capture_base(group: GroupRequest, *, dry_run: bool) -> dict[str, Any]:
    """Capture missing base inputs; never borrow the fixed-calibration budget."""
    suite = load_json(group.profile_suite)
    jobs = [_workload(group.raw, suite, workload) for workload in group.missing_base_workloads]
    attempts = base_attempts(group.raw)
    budget = group.base_budget
    plan = {"status": "needs_base_capture", "requirements": [], "base_capture_usage": profile_usage(attempts),
            "missing_base_workloads": list(group.missing_base_workloads),
            "base_capture_budget": asdict(budget) if budget else None, "base_jobs": []}
    for job in jobs:
        previous = [row for row in attempts if row["workload"] == job["workload"] and row["inputs"] == job["inputs"]]
        captured = next((row for row in reversed(previous) if row["mode"] == "capture" and row["status"] == "completed"), None)
        job["bundle"] = group.raw.get("forced_token_bundle") or (captured["forced_token_bundle"] if captured else None)
        job["modes"] = ["replay"] if job["bundle"] else ["capture", "replay"]
        plan["base_jobs"].append({"workload": job["workload"], "modes": job["modes"],
                                  "requests": len(job["modes"]) * job["counts"]["requests"],
                                  "tokens": len(job["modes"]) * job["counts"]["tokens"]})
    if dry_run or budget is None:
        plan["stop_reason"] = "dry_run" if dry_run else "base_budget_required"
        return plan
    if has_pending_capture(group.output_dir):
        plan.update(status="capture_incomplete", stop_reason="inspect recorded live group container before resuming")
        return plan
    ledger_path = group.output_dir / "base_capture_ledger.json"
    for job in jobs:
        for mode in job["modes"]:
            usage = profile_usage(attempts)
            counts = job["counts"]
            limits = [key for key, extra in (("server_starts", 1), ("requests", counts["requests"]), ("tokens", counts["tokens"]))
                      if usage[key] + extra > getattr(budget, key)]
            remaining = budget.wall_seconds - usage["wall_seconds"]
            if remaining <= 30:
                limits.append("wall_seconds")
            if limits:
                plan.update(status="base_budget_exhausted", limits=limits)
                return plan
            work_root = group.output_dir / "base_captures"
            work_root.mkdir(parents=True, exist_ok=True)
            output = Path(tempfile.mkdtemp(prefix=f"{mode}_", dir=work_root))
            config = copy.deepcopy(job["suite"])
            argv = config["matrix"]["inputs"][0]["bench"]["command"]
            for flag, field in (("--template", "template"), ("--config-specs", "config_specs")):
                path = output / f"{field}.json"
                write_json(path, job["inputs"][field])
                argv[argv.index(flag) + 1] = str(repo_relative_path(path))
            if mode == "capture":
                argv[argv.index("--forced-token-mode") + 1] = "capture"
                index = argv.index("--forced-token-plan")
                del argv[index:index + 2]
                config["profiling"] = {"enabled": False, "channels": []}
                config["run_root"] = "data/no_profile_runs/sglang"
                config["metadata"]["profile_mode"] = "forced_token_capture"
                config["metadata"]["full_dag_contract"] = "Not applicable to token capture; the subsequent replay produces source DAG traces."
            config["run_id"] = sanitize(f"{time.strftime('%Y%m%d_%H%M%S')}_base_{output.name}")
            suite_path = output / "suite.json"
            write_json(suite_path, config)
            command = [str(ROOT_DIR / "scripts/profile.sh"), str(repo_relative_path(suite_path))]
            row = {"workload": job["workload"], "inputs": job["inputs"], "mode": mode, "status": "running",
                   "container": f"markov-base-{os.getpid()}-{time.time_ns()}", "started_at_unix": time.time(),
                   "suite_dir": str(repo_relative_path(require_repo_path(config["run_root"]) / config["run_id"])),
                   "suite_config": str(repo_relative_path(suite_path)), "command_log": str(repo_relative_path(output / "command.log")),
                   "reserved_requests": counts["requests"], "reserved_tokens": counts["tokens"],
                   "budgeted_output_tokens_per_request": counts["output_tokens_per_request"]}
            if mode == "replay":
                resolve_forced_token_bundle_plan(require_repo_path(job["bundle"]), job["workload"])
                row["forced_token_bundle"] = job["bundle"]
                command.extend(("--forced-token-bundle", job["bundle"]))
            attempts.append(row)
            write_json(ledger_path, {"attempts": attempts, "usage": profile_usage(attempts)})
            print(f"base {job['workload']} {mode}: starting; remaining wall budget {remaining:.1f}s", flush=True)
            try:
                run_profile_attempt(row, command, remaining)
            finally:
                plan["base_capture_usage"] = profile_usage(attempts)
                write_json(ledger_path, {"attempts": attempts, "usage": plan["base_capture_usage"]})
            if row["status"] != "completed":
                plan.update(status=row["status"], failed_workload=job["workload"])
                return plan
            if mode == "capture":
                job["bundle"] = row["forced_token_bundle"]
    plan["status"] = "base_captured"
    return plan
