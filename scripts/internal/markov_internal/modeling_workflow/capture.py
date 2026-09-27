"""Budgeted group-local capture/replay through the public profiling entrypoint."""

from __future__ import annotations

from collections.abc import Callable, Iterator
from contextlib import contextmanager
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
from ..contracts.forced_token.quality import forced_token_quality_from_report

if TYPE_CHECKING:
    from .group import GroupRequest


# Leave time for the named container and its host client to stop after timeout.
CAPTURE_CLEANUP_RESERVE_SEC = 30
CONTROL_CAPTURE_STAGES = ("token_plan_capture", "profiled_replay", "light_replay")


@contextmanager
def recorded_capture(
    path: Path,
    ledger: dict[str, Any],
    row: dict[str, Any],
    usage: Callable[[list[dict[str, Any]]], dict[str, Any]],
    *,
    kind: str,
) -> Iterator[None]:
    """Persist a reserved attempt before launch and its inclusive usage on exit.

    The caller owns admission, execution and artifact validation, and mutates
    row with the outcome. Each acquisition family retains its own ledger.
    """

    started = time.monotonic()
    row.update(status="running", container=f"markov-{kind}-{os.getpid()}-{time.time_ns()}", started_at_unix=time.time())
    ledger["attempts"].append(row)
    ledger["usage"] = usage(ledger["attempts"])
    write_json(path, ledger)
    try:
        yield
    except BaseException as error:
        # Command completion is not capture completion: output validation may
        # still fail. Preserve an earlier process/cleanup failure if present.
        if row["status"] in {"running", "completed"}:
            row.update(
                status="invalid_capture_output" if row["status"] == "completed" else "interrupted",
                error=str(error),
            )
        raise
    finally:
        row["wall_seconds"] = time.monotonic() - started
        ledger["usage"] = usage(ledger["attempts"])
        write_json(path, ledger)


def calibration_inputs(paths: dict[str, str]) -> dict[str, Any]:
    """Load the two files that define the one logical calibration input."""

    return {key: load_json(require_repo_path(paths[key])) for key in ("template", "config_specs")}


def matching_calibration_profiles(attempts: list[dict], inputs: dict, *, stage: str | None = None) -> list[dict]:
    """Reuse successful identical work; different experiments still share the budget."""
    return [
        row
        for row in attempts
        if (
            completed_profile(row)
            if stage is None
            else row["status"] == "completed" and row.get("stage", row["mode"]) == stage
        )
        and "calibration_inputs" in row
        and calibration_inputs(row["calibration_inputs"]) == inputs
    ]


def profile_usage(attempts: list[dict[str, Any]]) -> dict[str, Any]:
    """Charge complete outer wall and conservative requests, including failures."""
    return {
        "wall_seconds": sum(row.get("wall_seconds", 0) for row in attempts),
        "server_starts": len(attempts),
        "requests": sum(row.get("charged_requests", row["reserved_requests"]) for row in attempts),
        "tokens": sum(row.get("charged_tokens", row["reserved_tokens"]) for row in attempts),
    }


def capture_usage(attempts: list[dict[str, Any]]) -> dict[str, Any]:
    """Account from recorded attempts, without reopening prior experiment inputs."""
    return {
        **profile_usage(attempts),
        "completed_profiles": sum(completed_profile(row) for row in attempts),
    }


def pending_capture_reason(attempts: list[dict[str, Any]], ledger_path: Path) -> str | None:
    """Describe an unfinished attempt using current container state, without changing accounting.

    A missing container does not establish output validity or cleanup completion.
    Keep the attempt unresolved; never turn a stale ledger into permission to retry.
    """

    for row in attempts:
        if row["status"] not in {"running", "cleanup_incomplete"}:
            continue
        container = row.get("container")
        if container:
            result = subprocess.run(
                ["docker", "inspect", "--type", "container", "--format", "{{.State.Status}}", container],
                capture_output=True,
                text=True,
                timeout=10,
            )
            state = result.stdout.strip() if result.returncode == 0 else result.stderr.strip()
        else:
            state = "container was not recorded"
        return (
            f"Unfinished capture in {ledger_path}: recorded={row['status']}, container={container}, observed={state}. "
            "Inspect the attempt and its accounting before resuming; the ledger alone does not prove a live capture."
        )
    return None


def pending_group_capture(output_dir: Path) -> str | None:
    for name in (
        "base_capture_ledger.json",
        "capture_ledger.json",
        "physical_capture_ledger.json",
        "cpu_captures/capture_ledger.json",
    ):
        path = output_dir / name
        if path.exists() and (reason := pending_capture_reason(load_json(path)["attempts"], path)):
            return reason
    return None


def completed_profile(row: dict[str, Any]) -> bool:
    """Admit successful replay or verified single-pass capture, never diagnostics."""
    return (
        row.get("stage") not in {"token_plan_capture", "light_replay"}
        and row["status"] == "completed"
        and (row["mode"] == "replay" or (row["mode"] == "capture" and row.get("profile_captured") is True))
    )


def capture_calibration(group: GroupRequest, definition: dict[str, Any], *, stage: str = "capture") -> dict[str, Any]:
    """Acquire or resume one stage; all stages/failures share the same budget."""

    if stage not in {"capture", *CONTROL_CAPTURE_STAGES}:
        raise ValueError("Unknown calibration acquisition stage")

    ledger_path = group.output_dir / "capture_ledger.json"
    ledger = load_json(ledger_path) if ledger_path.exists() else {"base_config": group.base_config, "attempts": []}
    attempts = ledger["attempts"]
    if reason := pending_group_capture(group.output_dir):
        return {"status": "capture_incomplete", "reason": reason}
    point = definition["point"]
    point_id = point["id"]
    inputs = {key: definition["files"][key] for key in ("template", "config_specs")}
    current_inputs = calibration_inputs(inputs)
    profiles = matching_calibration_profiles(attempts, current_inputs, stage=stage)
    mode = "capture" if stage in {"capture", "token_plan_capture"} else "replay"
    bundle = None
    replay_inputs = None
    if mode == "replay" and (stage == "light_replay" or not profiles):
        predecessors = matching_calibration_profiles(
            attempts,
            current_inputs,
            stage="profiled_replay" if stage == "light_replay" else "token_plan_capture",
        )
        if not predecessors:
            return dict(status="missing_capture_stage", stage=stage, usage=capture_usage(attempts))
        predecessor = predecessors[-1]
        replay_inputs = predecessor["calibration_inputs"]
        # A light replay corrects this full capture, not the most recent token
        # capture. A later seed may have produced different output tokens.
        bundle = require_repo_path(predecessor["forced_token_bundle"])
        if stage == "light_replay":
            profiles = [row for row in profiles if require_repo_path(row["forced_token_bundle"]) == bundle]
    if profiles:
        return {
            "status": "already_captured",
            "profile_manifest": profiles[-1]["profile_manifest"],
            "usage": capture_usage(attempts),
        }
    if bundle is not None and not bundle.is_file():
        raise ValueError("Recorded calibration token bundle is unavailable")
    usage = capture_usage(attempts)
    counts = definition["counts"]
    remaining, limits = group.budget.available_for(
        usage, server_starts=1, requests=counts["requests"], tokens=counts["tokens"]
    )
    if limits:
        return {"status": "budget_exhausted", "limits": limits, "usage": usage}

    work_root = group.output_dir / "captures"
    work_root.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix=f"{point_id}_{mode}_", dir=work_root))
    suite = load_json(require_repo_path(point["files"]["capture_suite"]))
    suite.setdefault("metadata", {})["profile_mode"] = "forced_token_" + mode
    argv = suite["bench"]["command"]
    if mode == "replay":
        argv[argv.index("--forced-token-mode") + 1] = "replay"
        argv.extend(("--forced-token-plan", "{forced_token_plan}"))
    if stage == "token_plan_capture":
        # This pass only records outputs for paired replay. Its trace is never
        # admitted as cost evidence; retain HTTP state gates, not profiler work.
        suite["profiling"] = {"enabled": False, "channels": []}
    elif stage == "light_replay":
        suite["profiling"].update(enabled=True, channels=["ld_preload"])
    runtime_inputs = {}
    for flag, key in (("--template", "template"), ("--config-specs", "config_specs")):
        index = argv.index(flag) + 1
        source = require_repo_path(argv[index])
        runtime_input = require_repo_path(replay_inputs[key]) if replay_inputs else output / source.name
        if replay_inputs is None:
            write_json(runtime_input, load_json(source))
        argv[index] = str(repo_relative_path(runtime_input))
        runtime_inputs[key] = str(repo_relative_path(runtime_input))
    suite["run_id"] = sanitize(f"{time.strftime('%Y%m%d_%H%M%S')}_{group.base_config}_{output.name}")
    suite_dir = require_repo_path(suite["run_root"]) / suite["run_id"]
    suite_path = output / "suite.json"
    write_json(suite_path, suite)
    command = [str(ROOT_DIR / "scripts/profile.sh"), str(repo_relative_path(suite_path))]
    if bundle is not None:
        command.extend(("--forced-token-bundle", str(repo_relative_path(bundle))))
    row = {
        "calibration_point": point_id,
        "page_size": point["page_size"],
        "mode": mode,
        "stage": stage,
        "suite_dir": str(repo_relative_path(suite_dir)),
        "suite_config": str(repo_relative_path(suite_path)),
        "command_log": str(repo_relative_path(output / "command.log")),
        "calibration_inputs": runtime_inputs,
        "reserved_requests": counts["requests"],
        "reserved_tokens": counts["tokens"],
        "budgeted_output_tokens_per_request": counts["output_tokens_per_request"],
    }
    if bundle is not None:
        row["forced_token_bundle"] = str(repo_relative_path(bundle))
    ledger["accounting"] = "all stages and failures share the cumulative budget; completed stages are reused"
    with recorded_capture(ledger_path, ledger, row, capture_usage, kind="calibration"):
        print(f"calibration {point_id} {stage}: starting; remaining wall budget {remaining:.1f}s", flush=True)
        run_profile_attempt(row, command, remaining)
    print(f"calibration {point_id} {stage}: {row['status']}, wall {row['wall_seconds']:.1f}s", flush=True)
    if row["status"] != "completed":
        return {"status": row["status"], "attempt": row, "usage": ledger["usage"]}
    return {"status": "captured", "profile_manifest": row["profile_manifest"], "usage": ledger["usage"]}


def run_profile_attempt(
    row: dict[str, Any], command: list[str], remaining: float, *, manifest_path: Path | None = None
) -> None:
    """Run a suite or explicit light replay; the caller owns its budget ledger."""
    try:
        run_container_attempt(row, command, remaining)
    finally:
        try:
            _capture_artifacts(row, manifest_path=manifest_path)
        except (OSError, ValueError, KeyError, TypeError) as error:
            row["artifact_error"] = str(error)
            if row["status"] == "completed":
                row["status"] = "invalid_capture_output"


def validate_light_replay(manifest: dict[str, Any], report: dict[str, Any]) -> None:
    """Admit a completed token replay measured only with LD_PRELOAD hooks."""

    quality = forced_token_quality_from_report(report)
    coverage = manifest["trace_channel_coverage"]
    if (
        manifest["status"] != "completed"
        or manifest.get("dry_run")
        or manifest.get("collection_errors")
        or report["status"] != "completed"
        or not quality["ready"]
        or quality["mode"] != "replay"
        or not coverage.get("ld_preload_trace_files")
        or coverage.get("torch_trace_files")
        or coverage.get("python_probe_trace_files")
    ):
        raise ValueError("Light capture lacks successful replay or LD-only traces")


def run_container_attempt(
    row: dict[str, Any], command: list[str], remaining: float, *, environment: dict[str, str] | None = None
) -> None:
    """Run one explicitly named acquisition container with a bounded lifetime."""
    process = None
    try:
        process = start_process(
            command,
            require_repo_path(row["command_log"]),
            {
                **os.environ,
                **(environment or {}),
                "TRACE_SIM_PROFILE_CONTAINER_NAME": row["container"],
                "TRACE_SIM_RUN_CONTAINER_NAME": row["container"],
            },
        )
        row["returncode"] = process.wait(timeout=remaining - CAPTURE_CLEANUP_RESERVE_SEC)
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
                stopped = subprocess.run(
                    ["docker", "stop", "--time", "10", row["container"]], capture_output=True, text=True, timeout=20
                )
                row["container_stop_returncode"] = stopped.returncode
                if stopped.returncode and "No such container" not in stopped.stderr:
                    row.update(status="cleanup_incomplete", cleanup_error=stopped.stderr.strip())
            except subprocess.TimeoutExpired:
                row["status"] = "cleanup_incomplete"
            stop_process(process, timeout_sec=5)


def _capture_artifacts(row: dict[str, Any], *, manifest_path: Path | None = None) -> None:
    """Read only this attempt's outputs; absent failure evidence remains unknown."""

    mode = row["mode"]
    summary = None
    if manifest_path is None:
        summary_path = require_repo_path(row["suite_dir"]) / "suite_result.json"
        summary = load_json(summary_path) if summary_path.exists() else {}
        manifests = [require_repo_path(path) for path in summary.get("profile_manifests", [])]
    else:
        manifests = [manifest_path]
    if len(manifests) == 1 and manifests[0].is_file():
        manifest = load_json(manifests[0])
        row["profile_manifest"] = str(repo_relative_path(manifests[0]))
        row["manifest_status"] = manifest["status"]
        row["profile_captured"] = bool(manifest.get("profiling", {}).get("enabled")) and all(
            manifest.get("trace_channel_coverage", {}).get(key, 0) > 0
            for key in ("torch_trace_files", "ld_preload_trace_files", "python_probe_trace_files")
        )
        reports = manifest.get("bench", {}).get("workload_report_files", [])
        if len(reports) == 1:
            report = load_json(require_repo_path(reports[0]["path"]))
            requests = [item for item in report["requests"] if item["kind"] == "request"]
            row["charged_requests"] = len(requests)
            if "budgeted_output_tokens_per_request" in row:
                input_tokens = sum(int(item["origin_input_count"]) for item in requests)
                row["charged_tokens"] = input_tokens + sum(
                    int(item["actual_output_count"])
                    if item.get("status") == "ok"
                    else row["budgeted_output_tokens_per_request"]
                    for item in requests
                )
            row["workload_status"] = report["status"]
            row["failure_reason"] = report.get("failure_reason")
        elif not reports and manifest.get("workload_started") is False:
            row.update(
                charged_requests=0,
                charged_tokens=0,
                request_accounting_evidence="terminal manifest: workload not started",
            )
    # Failed commands may still supply accounting evidence, but artifact parsing
    # must not replace the primary failure or make an incomplete run reusable.
    if row["status"] != "completed":
        return
    if (
        (summary is not None and (summary.get("status") != "completed" or summary.get("dry_run") is not False))
        or len(manifests) != 1
        or row.get("manifest_status") != "completed"
        or row.get("workload_status") != "completed"
    ):
        row.update(status="invalid_capture_output", error="successful real single-run evidence missing")
        return

    if mode == "capture":
        bundle = summary.get("generated_forced_token_bundle") or {}
        path = bundle.get("path")
        if not path or not require_repo_path(path).is_file():
            row.update(status="invalid_capture_output", error="capture did not produce a forced-token bundle")
            return
        row["forced_token_bundle"] = str(repo_relative_path(require_repo_path(path)))

    light = row.get("stage") == "light_replay"
    if light:
        validate_light_replay(manifest, report)
    if row.get("stage") == "profiled_replay":
        quality = forced_token_quality_from_report(report)
        if not quality["ready"] or quality["mode"] != "replay":
            row.update(status="invalid_capture_output", error="Calibration stage did not verify its token replay")
            return
    if (
        not light
        and (mode != "capture" or manifest.get("profiling", {}).get("enabled"))
        and not row["profile_captured"]
    ):
        row.update(status="invalid_capture_output", error="a required profiling channel has no trace")
        return
    if mode == "capture" and row["profile_captured"]:
        gates = [item for item in report["requests"] if item["kind"] in {"startup_gate", "barrier", "checkpoint"}]
        if not {"startup_gate", "checkpoint"} <= {item["kind"] for item in gates} or any(
            item["status"] != "ok" for item in gates
        ):
            row.update(
                status="invalid_capture_output", error="profiled token capture requires complete HiCache state gates"
            )
