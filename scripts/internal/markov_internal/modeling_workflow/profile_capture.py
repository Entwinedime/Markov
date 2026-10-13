"""Serial template capture shared by base preparation and independent measurements."""

from __future__ import annotations

import copy
from dataclasses import asdict, dataclass
from pathlib import Path
import tempfile
import time
from typing import Any, TYPE_CHECKING

from ..common.commands import command_tokens, replace_command_option
from ..common.io import load_json, write_json
from ..common.naming import sanitize
from ..common.paths import ROOT_DIR, repo_relative_path, require_repo_path
from ..contracts.forced_token.bundle import resolve_forced_token_bundle_plan
from ..contracts.forced_token.plan import load_forced_token_plan
from ..modeling.source_dag.profiles import ProfileRunRef
from ..profiling.suite import experiment_identity, filter_suite_experiments, profile_experiments
from ..workload_template.cli import parse_workload_command
from ..workload_template.expand import request_token_budget
from ..workload_template.schema import load_template
from .capture import ProfileBudget, pending_group_capture, profile_usage, recorded_capture, run_profile_attempt

if TYPE_CHECKING:
    from .group import GroupRequest


@dataclass(frozen=True)
class ProfileCaptureJob:
    config_id: str
    workload: str
    direct_replay: bool
    counts: dict[str, int]
    inputs: dict[str, Any]


def profile_token_plan(source: ProfileRunRef, bundle: str | None = None) -> Path | None:
    """Resolve a declared replay plan from this profile, not neighboring runs."""

    if bundle:
        return resolve_forced_token_bundle_plan(require_repo_path(bundle), source.input_id)

    args = parse_workload_command(load_json(source.config_path).get("bench", {}).get("command"))
    if args is None or args.forced_token_mode != "replay":
        return None

    path = require_repo_path(args.forced_token_plan)
    if load_forced_token_plan(path).get("workload_id") != source.input_id:
        raise ValueError(f"profile token plan belongs to another workload: {source.input_id}")

    return path


def base_attempts(raw: dict[str, Any]) -> list[dict[str, Any]]:
    path = require_repo_path(raw["output_dir"]) / "base_capture_ledger.json"
    return load_json(path)["attempts"] if path.exists() else []


def _workload(raw: dict[str, Any], suite: dict[str, Any], workload: str) -> ProfileCaptureJob:
    experiments = filter_suite_experiments(
        list(enumerate(profile_experiments(suite), start=1)),
        set(),
        selected_servers={raw["base_config"]},
        selected_inputs={workload},
    )
    if len(experiments) != 1:
        raise ValueError("base acquisition requires one declared experiment per base/workload")

    index, experiment = experiments[0]
    return profile_capture_job(
        experiment,
        index,
        config_id=raw["base_config"],
        workload=workload,
        forced_token_bundle=raw.get("forced_token_bundle"),
        purpose="base",
    )


def profile_capture_job(
    experiment: dict[str, Any],
    index: int,
    *,
    config_id: str,
    workload: str,
    forced_token_bundle: str | None = None,
    purpose: str,
) -> ProfileCaptureJob:
    """Prepare one explicit template workload; preserve its actual server overrides."""

    selected = copy.deepcopy(experiment)
    selected.pop("run_id", None)
    # Keep suite accounting and forced-token bundle output for this single run,
    # without reconstructing matrix axes and losing experiment-level overrides.
    selected["experiments"] = [{"id": experiment_identity(selected, index)}]
    selected.update(continue_on_error=False, name=f"{config_id}_{workload}_{purpose}")
    selected["server"]["startup_max_attempts"] = 1
    selected.setdefault("metadata", {}).update(
        config_id=config_id,
        workload_id=workload,
        purpose="Group base acquisition, separate from fixed calibration.",
        execution_contract="One declared base workload; serial and separately budgeted.",
    )
    if purpose != "base":
        selected["metadata"].update(
            purpose="Independent runtime performance measurements, reusable across prediction groups.",
            execution_contract="Declared sampling workload; serial and cumulatively budgeted.",
        )
        # Independent operator measurements need observed tensor work. The
        # base capture contract stays unchanged; same-capture CPU replay owns
        # correction of the added observation overhead.
        selected["profiling"].setdefault("torch", {})["record_shapes"] = True
    argv = command_tokens(selected["bench"]["command"])
    args = parse_workload_command(argv)
    if args is None or args.template is None:
        raise ValueError(
            "profile acquisition requires an executable template workload, not a corpus preparation command"
        )

    # Token recording is a workflow step, not a declaration users must encode
    # in their base config. Only an explicit replay plan bypasses recording.
    direct_replay = (
        args.forced_token_mode == "replay"
        and bool(args.forced_token_plan)
        and args.forced_token_plan != "{forced_token_plan}"
    )
    if direct_replay and forced_token_bundle:
        raise ValueError("replay must use either an explicit token plan or a bundle, not both")

    replace_command_option(argv, "--forced-token-mode", "replay")
    if not direct_replay:
        replace_command_option(argv, "--forced-token-plan", "{forced_token_plan}")
    if not args.output_dir.startswith("{bench_dir}/"):
        # Both passes need fresh reports. Do not overwrite a user-specified
        # report directory, or the source report during a later light replay.
        replace_command_option(argv, "--output-dir", "{bench_dir}/base_workload")
    selected["bench"]["command"] = argv

    template = load_template(require_repo_path(args.template))
    return ProfileCaptureJob(
        config_id=config_id,
        workload=workload,
        direct_replay=direct_replay,
        counts=request_token_budget(template),
        inputs={
            "suite": selected,
            "template": dict(template.data),
            "config_specs": load_json(require_repo_path(args.config_specs)) if args.config_id else None,
            "forced_token_bundle": forced_token_bundle,
        },
    )


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

        inputs = _workload(raw, suite, workload).inputs
        row = next((row for row in reversed(rows) if row["inputs"] == inputs), None)
        if row:
            result.append(require_repo_path(row["profile_manifest"]))
    return result


def capture_base(group: GroupRequest, *, dry_run: bool) -> dict[str, Any]:
    """Capture missing base inputs; never borrow the fixed-calibration budget."""
    suite = load_json(group.profile_suite)
    jobs = [_workload(group.raw, suite, workload) for workload in group.missing_base_workloads]
    result = capture_profile_jobs(jobs, group.output_dir, group.base_budget, dry_run=dry_run, purpose="base")
    result["status"] = {
        "needs_profile_capture": "needs_base_capture",
        "profiles_captured": "base_captured",
        "capture_budget_exhausted": "base_budget_exhausted",
    }.get(result["status"], result["status"])
    result["base_capture_usage"] = result.pop("usage")
    result["base_capture_budget"] = result.pop("budget")
    result["base_jobs"] = result.pop("jobs")
    result["missing_base_workloads"] = list(group.missing_base_workloads)
    if result.get("stop_reason") == "capture_budget_required":
        result["stop_reason"] = "base_budget_required"

    return result


def capture_profile_jobs(
    jobs: list[ProfileCaptureJob],
    output_dir: Path,
    budget: ProfileBudget | None,
    *,
    dry_run: bool,
    purpose: str,
) -> dict[str, Any]:
    """Record tokens then profile each job; all jobs share one cumulative ledger.

    Only completed captures of identical declared inputs are reused. New traces
    still hand off through manifests; token-only passes are never measurements.
    The caller chooses base or independent ownership, not a target identity.
    """

    ledger_path = output_dir / f"{purpose}_capture_ledger.json"
    attempts = load_json(ledger_path)["attempts"] if ledger_path.exists() else []
    plan = {
        "status": "needs_profile_capture",
        "usage": profile_usage(attempts),
        "budget": asdict(budget) if budget else None,
        "jobs": [],
        "profile_manifests": [],
    }
    pending = []
    for job in jobs:
        previous = [row for row in attempts if row["workload"] == job.workload and row["inputs"] == job.inputs]
        replay = next(
            (row for row in reversed(previous) if row["mode"] == "replay" and row["status"] == "completed"), None
        )
        if replay:
            plan["profile_manifests"].append(replay["profile_manifest"])
            continue

        captured = next(
            (row for row in reversed(previous) if row["mode"] == "capture" and row["status"] == "completed"), None
        )
        bundle = job.inputs["forced_token_bundle"] or (captured["forced_token_bundle"] if captured else None)
        modes = ["replay"] if bundle or job.direct_replay else ["capture", "replay"]
        pending.append((job, bundle, modes))
        plan["jobs"].append(
            {
                "config_id": job.config_id,
                "workload": job.workload,
                "modes": modes,
                "requests": len(modes) * job.counts["requests"],
                "tokens": len(modes) * job.counts["tokens"],
            }
        )
    if not pending:
        return dict(plan, status="profiles_captured")
    if dry_run or budget is None:
        plan["stop_reason"] = "dry_run" if dry_run else "capture_budget_required"
        return plan
    if reason := pending_group_capture(output_dir):
        plan.update(status="capture_incomplete", stop_reason=reason)
        return plan
    for job, bundle, modes in pending:
        for mode in modes:
            usage = profile_usage(attempts)
            counts = job.counts
            remaining, limits = budget.available_for(
                usage, server_starts=1, requests=counts["requests"], tokens=counts["tokens"]
            )
            if limits:
                plan.update(status="capture_budget_exhausted", limits=limits)
                return plan
            work_root = output_dir / f"{purpose}_captures"
            work_root.mkdir(parents=True, exist_ok=True)
            output = Path(tempfile.mkdtemp(prefix=f"{mode}_", dir=work_root))
            config = copy.deepcopy(job.inputs["suite"])
            argv = config["bench"]["command"]
            for flag, field in (("--template", "template"), ("--config-specs", "config_specs")):
                if job.inputs[field] is None:
                    continue

                path = output / f"{field}.json"
                write_json(path, job.inputs[field])
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
            config["run_id"] = sanitize(f"{time.strftime('%Y%m%d_%H%M%S')}_{purpose}_{output.name}")
            suite_path = output / "suite.json"
            write_json(suite_path, config)
            command = [str(ROOT_DIR / "scripts/profile.sh"), str(repo_relative_path(suite_path))]
            row = {
                "workload": job.workload,
                "inputs": job.inputs,
                "mode": mode,
                "suite_dir": str(repo_relative_path(require_repo_path(config["run_root"]) / config["run_id"])),
                "suite_config": str(repo_relative_path(suite_path)),
                "command_log": str(repo_relative_path(output / "command.log")),
                "reserved_requests": counts["requests"],
                "reserved_tokens": counts["tokens"],
                "budgeted_output_tokens_per_request": counts["output_tokens_per_request"],
            }
            if mode == "replay" and bundle:
                resolve_forced_token_bundle_plan(require_repo_path(bundle), job.workload)
                row["forced_token_bundle"] = bundle
                command.extend(("--forced-token-bundle", bundle))
            try:
                with recorded_capture(ledger_path, {"attempts": attempts}, row, profile_usage, kind=purpose):
                    print(
                        f"{purpose} {job.config_id}/{job.workload} {mode}: starting; remaining wall budget {remaining:.1f}s",
                        flush=True,
                    )
                    run_profile_attempt(row, command, remaining)
            finally:
                plan["usage"] = profile_usage(attempts)
            if row["status"] != "completed":
                plan.update(status=row["status"], failed_workload=job.workload, attempt=row)
                return plan
            if mode == "capture":
                bundle = row["forced_token_bundle"]
            else:
                plan["profile_manifests"].append(row["profile_manifest"])
    plan["status"] = "profiles_captured"
    return plan
