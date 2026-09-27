"""Budgeted lightweight base replay using the normal profiling container lifecycle."""

from __future__ import annotations

from pathlib import Path
import tempfile

from ..common.io import load_json, write_json
from ..common.manifest import workload_report_path
from ..common.paths import ROOT_DIR, require_repo_path, repo_relative_path
from .calibration.light_capture import light_capture_config
from .capture import (
    pending_capture_reason,
    profile_usage,
    recorded_capture,
    run_profile_attempt,
)
from .group import ProfileBudget


def capture_light_base(pair: dict, plan: dict, *, dry_run: bool = False) -> Path | None:
    """Return a completed capture, or record an expected stop in the CPU plan.

    Budget exhaustion, failed commands and invalid profile artifacts return None.
    Unexpected exceptions propagate; all started attempts remain charged.
    """

    root = require_repo_path(plan["capture_root"])
    ledger = root / "capture_ledger.json"
    attempts = load_json(ledger)["attempts"] if ledger.exists() else []
    plan["capture_usage"] = profile_usage(attempts)
    if reason := pending_capture_reason(attempts, ledger):
        plan.update(status="capture_incomplete", stop_reason=reason)
        return None
    for row in reversed(attempts):
        if (
            row["status"] == "completed"
            and row["source_manifest"] == pair["profile_manifest"]
            and row["forced_token_bundle"] == plan["forced_token_bundle"]
        ):
            return require_repo_path(row["light_manifest"])
    source = require_repo_path(pair["profile_manifest"])
    bundle = require_repo_path(plan["forced_token_bundle"]) if plan["forced_token_bundle"] else None
    report = load_json(workload_report_path(load_json(source)))
    requests = [row for row in report["requests"] if row["kind"] == "request"]
    counts = dict(
        server_starts=1,
        requests=len(requests),
        tokens=sum(row["origin_input_count"] + row["forced_output_count"] for row in requests),
    )
    plan["capture_required"] = counts
    if plan["capture_budget"] is None:
        plan.update(
            status="needs_capture_inputs",
            stop_reason="Provide a matching light_manifest or declare cpu_service_capture_budget for base replay.",
        )
        return None

    budget = ProfileBudget(**plan["capture_budget"])
    remaining, limits = budget.available_for(plan["capture_usage"], **counts)
    if limits:
        plan.update(status="cpu_capture_budget_exhausted", limits=limits)
        return None
    if dry_run:
        plan["status"] = "needs_preparation"
        return None

    root.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="light_", dir=root))
    config = light_capture_config(source, output, bundle)
    manifest_path = require_repo_path(config["run_root"]) / config["run_id"] / "profile_manifest.json"
    config_path = output / "config.json"
    write_json(config_path, config)
    row = dict(
        mode="replay",
        stage="light_replay",
        source_manifest=pair["profile_manifest"],
        workload=report["workload_id"],
        forced_token_bundle=plan["forced_token_bundle"],
        command_log=str(repo_relative_path(output / "command.log")),
        config_path=str(repo_relative_path(config_path)),
        reserved_requests=counts["requests"],
        reserved_tokens=counts["tokens"],
    )
    try:
        with recorded_capture(ledger, dict(attempts=attempts), row, profile_usage, kind="cpu"):
            run_profile_attempt(
                row,
                [
                    str(ROOT_DIR / "scripts/profile.sh"),
                    str(repo_relative_path(config_path)),
                ],
                remaining,
                manifest_path=manifest_path,
            )
            if row["status"] == "completed" and not any((output / "timing").glob("*.jsonl")):
                row.update(status="invalid_capture_output", error="Light capture lacks CPU timings")
            if row["status"] != "completed":
                plan.update(status="cpu_capture_failed", attempt=row)
                return None

            row["light_manifest"] = str(repo_relative_path(manifest_path))
    finally:
        plan["capture_usage"] = profile_usage(attempts)
    return manifest_path
