"""Budgeted group-local capture/replay through the public profiling entrypoint."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import time
from typing import Any, TYPE_CHECKING

from ..common.io import load_json, write_json
from ..common.naming import sanitize
from ..common.paths import ROOT_DIR, repo_relative_path, require_repo_path
from ..common.process import start_process, stop_process
from ..contracts.forced_token.bundle import resolve_forced_token_bundle_plan
from ..workload_template.schema import load_template
from ..workload_template.expand import expand_template
from ..workload_template.executor import _validate_replay_plan
if TYPE_CHECKING:
    from .group import GroupRequest


def calibration_inputs(paths: dict[str, str]) -> dict[str, Any]:
    """Load the two files that define the one logical calibration input."""

    return {key: load_json(require_repo_path(paths[key])) for key in ("template", "config_specs")}


def reusable_token_bundle(attempts: list[dict[str, Any]], template: dict[str, Any]) -> str | None:
    """Reuse verified output tokens from an earlier run of the same calibration."""

    for row in reversed(attempts):
        if row["status"] != "completed" or "forced_token_bundle" not in row or "calibration_inputs" not in row:
            continue
        saved = load_template(require_repo_path(row["calibration_inputs"]["template"]))
        if saved.data != template:
            continue
        try:
            plan = resolve_forced_token_bundle_plan(require_repo_path(row["forced_token_bundle"]), saved.workload_id)
            _validate_replay_plan(expand_template(saved, None), Path(plan.plan_path))
        except (OSError, ValueError):
            continue
        return row["forced_token_bundle"]
    return None


def profile_usage(attempts: list[dict[str, Any]]) -> dict[str, Any]:
    """Charge complete outer wall and conservative requests, including failures."""
    return {"wall_seconds": sum(row.get("wall_seconds", 0) for row in attempts),
            "server_starts": len(attempts),
            "requests": sum(row.get("charged_requests", row["reserved_requests"]) for row in attempts),
            "tokens": sum(row.get("charged_tokens", row["reserved_tokens"]) for row in attempts)}


def capture_usage(attempts: list[dict[str, Any]]) -> dict[str, Any]:
    identities: list[dict[str, Any]] = []
    for row in attempts:
        if "calibration_inputs" not in row:
            continue
        value = calibration_inputs(row["calibration_inputs"])
        if value not in identities:
            identities.append(value)
    return {
        **profile_usage(attempts),
        "completed_profiles": sum(completed_profile(row) for row in attempts),
        "distinct_calibration_inputs": len(identities),
    }


def has_pending_capture(output_dir: Path) -> bool:
    for name in ("base_capture_ledger.json", "capture_ledger.json", "physical_capture_ledger.json"):
        path = output_dir / name
        if path.exists() and any(row["status"] in {"running", "cleanup_incomplete"} for row in load_json(path)["attempts"]):
            return True
    return False


def completed_profile(row: dict[str, Any]) -> bool:
    """Admit successful replay or verified single-pass capture, never diagnostics."""
    return row["status"] == "completed" and (row["mode"] == "replay" or (
        row["mode"] == "capture" and row.get("profile_captured") is True))


def capture_calibration(group: GroupRequest, definition: dict[str, Any], point_id: str) -> dict[str, Any]:
    """Acquire one repeat at a declared endpoint under the cumulative budget."""

    ledger_path = group.output_dir / "capture_ledger.json"
    ledger = load_json(ledger_path) if ledger_path.exists() else {"base_config": group.base_config, "attempts": []}
    attempts = ledger["attempts"]
    if has_pending_capture(group.output_dir):
        return {"status": "capture_incomplete", "reason": "inspect recorded live container before resuming"}
    points = {point["id"]: point for point in definition["points"]}
    if point_id not in points:
        raise ValueError(f"unknown fixed calibration endpoint: {point_id}")
    point = points[point_id]
    name = point_id
    inputs = {key: definition["files"][key] for key in ("template", "config_specs")}
    current_inputs = calibration_inputs(inputs)
    existing_inputs = [calibration_inputs(row["calibration_inputs"]) for row in attempts if "calibration_inputs" in row]
    if any(value != current_inputs for value in existing_inputs):
        return {"status": "calibration_input_conflict", "usage": capture_usage(attempts)}
    profiles = [row for row in attempts if completed_profile(row) and row.get("calibration_point") == point_id]
    if len(profiles) >= group.budget.repeats:
        return {"status": "already_captured", "profile_manifest": profiles[-1]["profile_manifest"],
                "usage": capture_usage(attempts)}
    bundle = reusable_token_bundle(attempts, current_inputs["template"])
    template = load_template(require_repo_path(definition["files"]["template"]))
    compiled = expand_template(template, None)
    if bundle:
        try:
            token_plan = resolve_forced_token_bundle_plan(require_repo_path(bundle), compiled.template.workload_id)
            _validate_replay_plan(compiled, Path(token_plan.plan_path))
        except (OSError, ValueError):
            return {"status": "repeat_inputs_changed", "usage": capture_usage(attempts)}
    mode = "replay" if bundle else "capture"
    usage = capture_usage(attempts)
    remaining = group.budget.wall_seconds - usage["wall_seconds"]
    limits = [key for key, extra in (("server_starts", 1), ("requests", definition["requests_per_run"]),
              ("tokens", definition["tokens_per_run"])) if usage[key] + extra > getattr(group.budget, key)]
    if remaining <= 30:
        limits.append("wall_seconds")
    if limits:
        return {"status": "budget_exhausted", "limits": limits, "usage": usage}

    work_root = group.output_dir / "captures"
    work_root.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix=f"{name}_{mode}_", dir=work_root))
    suite = load_json(require_repo_path(point["files"]["capture_suite" if mode == "capture" else "profile_suite"]))
    argv = suite["matrix"]["inputs"][0]["bench"]["command"]
    runtime_inputs = {}
    for flag, key in (("--template", "template"), ("--config-specs", "config_specs")):
        index = argv.index(flag) + 1
        source = require_repo_path(argv[index])
        runtime_input = output / source.name
        write_json(runtime_input, load_json(source))
        argv[index] = str(repo_relative_path(runtime_input))
        runtime_inputs[key] = str(repo_relative_path(runtime_input))
    suite["run_id"] = sanitize(f"{time.strftime('%Y%m%d_%H%M%S')}_{group.base_config}_{output.name}")
    suite_dir = require_repo_path(suite["run_root"]) / suite["run_id"]
    suite_path = output / "suite.json"
    write_json(suite_path, suite)
    command = [str(ROOT_DIR / "scripts/profile.sh"), str(repo_relative_path(suite_path))]
    if bundle:
        command.extend(("--forced-token-bundle", bundle))
    row = {
        "calibration": name,
        "calibration_point": point_id,
        "page_size": point["page_size"],
        "mode": mode,
        "status": "running",
        "container": f"markov-calibration-{os.getpid()}-{time.time_ns()}",
        "suite_dir": str(repo_relative_path(suite_dir)),
        "suite_config": str(repo_relative_path(suite_path)),
        "command_log": str(repo_relative_path(output / "command.log")),
        "started_at_unix": time.time(),
        "calibration_inputs": runtime_inputs,
        "reserved_requests": definition["requests_per_run"],
        "reserved_tokens": definition["tokens_per_run"],
        "budgeted_output_tokens_per_request": max(request.max_new_tokens for request in compiled.requests),
    }
    if profiles:
        row["repeat_of"] = profiles[0]["profile_manifest"]
    if bundle:
        row["forced_token_bundle"] = bundle
    attempts.append(row)
    ledger["usage"] = capture_usage(attempts)
    write_json(ledger_path, ledger)
    print(f"fixed calibration {point_id} {mode}: starting; remaining wall budget {remaining:.1f}s", flush=True)
    try:
        run_profile_attempt(row, command, remaining)
    finally:
        ledger["usage"] = capture_usage(attempts)
        ledger["accounting"] = "all attempts are charged; endpoint repeats use the same fixed workload and declared configs"
        write_json(ledger_path, ledger)
    print(f"fixed calibration {point_id} {mode}: {row['status']}, wall {row['wall_seconds']:.1f}s", flush=True)
    if row["status"] != "completed":
        return {"status": row["status"], "attempt": row, "usage": ledger["usage"]}
    return {"status": "captured", "profile_manifest": row["profile_manifest"], "usage": ledger["usage"]}


def run_profile_attempt(row: dict[str, Any], command: list[str], remaining: float) -> None:
    """Run one base or fixed-calibration profile; the caller owns its budget ledger."""
    try:
        run_container_attempt(row, command, remaining)
    finally:
        try:
            _capture_artifacts(row, row["mode"])
        except (OSError, ValueError, KeyError, TypeError) as error:
            row["artifact_error"] = str(error)
            if row["status"] != "cleanup_incomplete":
                row["status"] = "invalid_capture_output"


def run_container_attempt(row: dict[str, Any], command: list[str], remaining: float,
                          *, environment: dict[str, str] | None = None) -> None:
    """Run one explicitly named acquisition container with a bounded lifetime."""
    started = time.monotonic()
    process = None
    try:
        process = start_process(command, require_repo_path(row["command_log"]),
                                {**os.environ, **(environment or {}), "TRACE_SIM_PROFILE_CONTAINER_NAME": row["container"],
                                 "TRACE_SIM_RUN_CONTAINER_NAME": row["container"]})
        row["returncode"] = process.wait(timeout=remaining - 30)
        row["status"] = "completed" if row["returncode"] == 0 else "failed"
    except subprocess.TimeoutExpired:
        row["status"] = "budget_timeout"
    except BaseException as error:
        row.update(status="interrupted", error=str(error))
        raise
    finally:
        # Stopping the client alone does not stop inference in its container.
        if row["status"] != "completed":
            try:
                stopped = subprocess.run(["docker", "stop", "--time", "10", row["container"]],
                                         capture_output=True, text=True, timeout=20)
                row["container_stop_returncode"] = stopped.returncode
                if stopped.returncode and "No such container" not in stopped.stderr:
                    row.update(status="cleanup_incomplete", cleanup_error=stopped.stderr.strip())
            except subprocess.TimeoutExpired:
                row["status"] = "cleanup_incomplete"
            stop_process(process, timeout_sec=5)
        row["wall_seconds"] = time.monotonic() - started


def _capture_artifacts(row: dict[str, Any], mode: str) -> None:
    """Read only this attempt's outputs; absent failure evidence remains unknown."""

    suite_dir = require_repo_path(row["suite_dir"])
    summary_path = suite_dir / "suite_result.json"
    summary = load_json(summary_path) if summary_path.exists() else {}
    manifests = list(suite_dir.glob("*/profile_manifest.json"))
    if len(manifests) == 1:
        manifest = load_json(manifests[0])
        row["profile_manifest"] = str(repo_relative_path(manifests[0]))
        row["manifest_status"] = manifest["status"]
        row["profile_captured"] = bool(manifest.get("profiling", {}).get("enabled")) and all(
            manifest.get("trace_channel_coverage", {}).get(key, 0) > 0
            for key in ("torch_trace_files", "ld_preload_trace_files", "python_probe_trace_files"))
        row["indexed_trace_bytes"] = sum(int(item.get("bytes") or 0)
                                 for section in ("trace", "sidecar") for items in manifest.get(section, {}).values()
                                 if isinstance(items, list) for item in items if isinstance(item, dict))
        row["trace_bytes"] = sum(path.stat().st_size for path in (manifests[0].parent / "trace").rglob("*") if path.is_file())
        reports = list((manifests[0].parent / "bench").glob("**/workload_report.json"))
        if len(reports) == 1:
            report = load_json(reports[0])
            requests = [item for item in report["requests"] if item["kind"] == "request"]
            row["observed_requests"] = len(requests)
            row["submitted_input_tokens"] = sum(int(item.get("origin_input_count", 0)) for item in requests)
            row["returned_output_tokens"] = sum(int(item.get("actual_output_count", 0)) for item in requests)
            row["charged_requests"] = len(requests)
            if "budgeted_output_tokens_per_request" in row:
                row["charged_tokens"] = row["submitted_input_tokens"] + sum(
                    int(item.get("actual_output_count", 0)) if item.get("status") == "ok"
                    else row["budgeted_output_tokens_per_request"] for item in requests)
            row["workload_status"] = report["status"]
            row["failure_reason"] = report.get("failure_reason")
        elif not reports and manifest.get("workload_started") is False:
            row.update(observed_requests=0, charged_requests=0, charged_tokens=0,
                       request_accounting_evidence="terminal manifest: workload not started")
    if row["status"] == "completed":
        if (summary.get("status") != "completed" or summary.get("dry_run") is not False or len(manifests) != 1
                or row.get("manifest_status") != "completed" or row.get("workload_status") != "completed"):
            row.update(status="invalid_capture_output", error="successful real single-run suite evidence missing")
        elif mode == "capture":
            bundle = summary.get("generated_forced_token_bundle") or {}
            path = bundle.get("path")
            if path and require_repo_path(path).is_file():
                row["forced_token_bundle"] = str(repo_relative_path(require_repo_path(path)))
            else:
                row.update(status="invalid_capture_output", error="capture did not produce a forced-token bundle")
        if row["status"] == "completed" and (mode != "capture" or manifest.get("profiling", {}).get("enabled")) and not row["profile_captured"]:
            row.update(status="invalid_capture_output", error="a required profiling channel has no trace")
        if row["status"] == "completed" and mode == "capture" and row["profile_captured"]:
            gates = [item for item in report["requests"] if item["kind"] in {"startup_gate", "barrier", "checkpoint"}]
            if not {"startup_gate", "checkpoint"} <= {item["kind"] for item in gates} or any(item["status"] != "ok" for item in gates):
                row.update(status="invalid_capture_output", error="profiled token capture requires complete HiCache state gates")
