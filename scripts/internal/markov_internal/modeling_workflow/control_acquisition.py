"""Shared control acquisition through the existing capture, CPU and export tools."""

from __future__ import annotations

from collections.abc import Sequence
import subprocess
from pathlib import Path
from typing import TYPE_CHECKING

from ..common.io import load_json, write_json
from ..common.paths import ROOT_DIR, require_repo_path, repo_relative_path
from .capture import CONTROL_CAPTURE_STAGES, capture_calibration
from .control_calibrations import control_calibrations
from .group_cpu_service import prepare_group_cpu_service
from .planning.profile_runs import discover_profile_runs, parse_server_command_flags
from .types import ProfileRunRef

if TYPE_CHECKING:
    from .group import GroupRequest


PREFETCH_CAPTURE_POLICIES = {
    "local_return": "best_effort",
    "best_effort": "best_effort",
    "wait_complete": "wait_complete",
}


def pending_control_experiments(needs: Sequence[dict], attempted: set[str]) -> list[str]:
    """Map execution demands to shared experiments, with queries exported last.

    The caller owns the attempt lifecycle. Unknown programs stay unresolved;
    they are not redirected to a different measurement.
    """
    components = {need["component"] for need in needs}
    operations = [
        name
        for name in ("eviction_locked_candidate", "release_regular")
        if "execution_control/" + name in components and name not in attempted
    ]
    waits = {
        PREFETCH_CAPTURE_POLICIES[coordinate["program"]]
        for need in needs
        if need["component"] == "execution_control/prefetch"
        for coordinate in need["coordinates"]
        if coordinate["program"] in PREFETCH_CAPTURE_POLICIES
    }
    operations.extend(sorted(waits - attempted))

    # The wait experiment may publish a paired trace that also supplies queries.
    if "execution_control/prefetch_query" in components and "prefetch_query" not in attempted:
        operations.append("prefetch_query")
    return operations


def prepare_control_calibration(group: GroupRequest, demand: str) -> dict:
    """Resolve a shared control demand, then capture and export its normal CPU costs."""
    # Materialization imports group definitions; keep that dependency local.
    from .fixed_calibration import materialize_fixed_calibration

    if demand == "prefetch_query":
        operation = "prefetch_query"
    elif demand == "release_regular":
        operation = "release_host"
    else:
        operation = "prefetch_wait"
    if operation == "prefetch_query":
        # Group admission already checks these independent manifests and their
        # environment. Re-export with current code; do not reuse old parameters.
        for path in sorted(set(group.raw["control_calibrations"]["prefetch_wait"].values())):
            existing = load_json(require_repo_path(path))
            if existing.get("cost_basis") != "paired_cpu_service" or not existing.get("cpu_service_file"):
                continue
            (profile,) = discover_profile_runs((require_repo_path(existing["source_manifest"]),))
            return _export_control(
                group,
                profile,
                require_repo_path(existing["cpu_service_file"]),
                group.output_dir / "control_calibration" / "shared_query",
                operation,
            )

    if demand == "release_regular":
        definition = materialize_fixed_calibration(group, release_only=True)
    else:
        policy = "wait_complete" if demand == "prefetch_query" else PREFETCH_CAPTURE_POLICIES[demand]
        definition = materialize_fixed_calibration(group, prefetch_policy=policy)

    manifests = {}
    for stage in CONTROL_CAPTURE_STAGES:
        result = capture_calibration(group, definition, stage=stage)
        if result["status"] not in {"captured", "already_captured"}:
            return dict(result, stage=stage)
        manifests[stage] = result["profile_manifest"]
    source = require_repo_path(manifests["profiled_replay"])
    (profile,) = discover_profile_runs((source,))
    flags = parse_server_command_flags(profile.run_dir / "server_cmd.txt")
    tp = int(flags.get("tp_size", "1"))
    ledger_path = group.output_dir / "capture_ledger.json"
    ledger = load_json(ledger_path)
    captures = {row["profile_manifest"]: row for row in ledger["attempts"] if "profile_manifest" in row}
    full, light = (captures[manifests[stage]] for stage in ("profiled_replay", "light_replay"))
    # Attempt directories already distinguish actual measurements. Bind both
    # CPU correction and exported costs to the pair, not the reusable point name.
    output = group.output_dir / "control_calibration"
    for row in (full, light):
        output /= require_repo_path(row["suite_config"]).parent.name
    pair = dict(
        profile_manifest=manifests["profiled_replay"],
        light_manifest=manifests["light_replay"],
        tp_size=tp,
        output_dir=str(repo_relative_path(output / "cpu")),
    )
    services = prepare_group_cpu_service(dict(pairs=[pair], existing_services=[]))
    full["cpu_service_cost"] = str(repo_relative_path(services[0]))
    write_json(ledger_path, ledger)
    return _export_control(group, profile, services[0], output, operation)


def _export_control(group: GroupRequest, profile: ProfileRunRef, service: Path, output: Path, operation: str) -> dict:
    # Export only the required operation, not every operation this workload
    # happens to contain. The paired capture is shared by the whole group.
    path = output / (operation + ".json")
    subprocess.run(
        [
            str(ROOT_DIR / "scripts/model.sh"),
            "export-hicache-operation-costs",
            "--profile-manifest",
            str(profile.manifest_path),
            "--cpu-service-cost",
            str(service),
            "--page-size",
            str(profile.hicache_config["page_size"]),
            "--operation",
            operation,
            "--output-dir",
            str(output),
        ],
        cwd=ROOT_DIR,
        check=True,
    )

    value = str(repo_relative_path(path))
    collection = (
        {"prefetch_wait": {profile.hicache_config["prefetch_policy"]: value}}
        if operation == "prefetch_wait"
        else {operation: value}
    )
    index = group.output_dir / "control_calibrations.json"
    write_json(index, control_calibrations(load_json(index) if index.exists() else {}, collection))
    return dict(
        status="control_calibrated",
        profile_manifest=str(repo_relative_path(profile.manifest_path)),
        control_calibrations=collection,
    )
