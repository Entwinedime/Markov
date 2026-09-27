"""Shared control acquisition through the existing capture, CPU and export tools."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING

from ..common.io import load_json, write_json
from ..common.paths import ROOT_DIR, require_repo_path, repo_relative_path
from .capture import CONTROL_CAPTURE_STAGES, capture_calibration
from .control_calibrations import control_calibrations
from .group_cpu_service import prepare_group_cpu_service
from .planning.profile_runs import discover_profile_runs, parse_server_command_flags

if TYPE_CHECKING:
    from .group import GroupRequest


def prepare_control_calibration(group: GroupRequest, plan: dict) -> dict:
    definition = plan["definition"]
    point = definition["point"]
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
    # Export only the required operation, not every operation this workload
    # happens to contain. The paired capture is shared by the whole group.
    operation = plan["operation"]
    path = output / (operation + ".json")
    subprocess.run(
        [
            str(ROOT_DIR / "scripts/model.sh"),
            "export-hicache-operation-costs",
            "--profile-manifest",
            str(source),
            "--cpu-service-cost",
            str(services[0]),
            "--page-size",
            str(point["page_size"]),
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
        status="control_calibrated", profile_manifest=manifests["profiled_replay"], control_calibrations=collection
    )
