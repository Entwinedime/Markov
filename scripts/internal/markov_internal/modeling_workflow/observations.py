"""Extract base and explicitly sourced, group-shared calibration observations."""

from __future__ import annotations

import argparse
from tempfile import TemporaryDirectory
from pathlib import Path
from typing import Any

from ..common.io import load_json, write_json
from ..common.paths import repo_relative_path, require_repo_path, running_in_modeling_container
from ..modeling.backend import append_option, execute_trace_graph, trace_graph_executable
from ..modeling.workload import WorkloadWindow, controlled_request_window
from .capture import completed_profile
from .io_model_builder import prepare_model
from .group import GroupRequest, environments_match, source_environment
from .group_cpu_service import cpu_service_plan, prepare_group_cpu_service
from .context import cpu_service_inputs
from .planning.profile_runs import discover_profile_runs
from .types import ProfileRunRef


def _source_command(
    source: ProfileRunRef,
    summary_path: Path,
    window: WorkloadWindow,
    threads: int,
    cpu_service: Path | None = None,
) -> list[str]:
    command = [
        str(trace_graph_executable()),
        "--profile-manifest",
        str(source.manifest_path),
        "--run-summary",
        str(summary_path),
        "--source-observations-only",
        "--trace-channels",
        "torch,ld_preload,python_probe",
        "--threads",
        str(threads),
        "--file-threads",
        str(threads),
    ]
    append_option(command, "--trace-window-start-us", window.start_ns // 1000)
    append_option(command, "--trace-window-end-us", window.end_ns // 1000)
    if cpu_service is not None:
        command.extend(("--cpu-service-cost", str(cpu_service)))
    return command


def scan_observations(
    source: ProfileRunRef, scratch_root: Path, *, role: str, threads: int = 4, cpu_service: Path | None = None
) -> dict[str, Any]:
    """Extract observed work/cost from a source profile without a target config."""

    scratch_root.mkdir(parents=True, exist_ok=True)
    window = source.workload_window
    if window is None:
        raise ValueError(f"model input has no declared formal workload window: {source.manifest_path}")
    io_window = controlled_request_window(window) if role == "calibration" else window
    # Only extracted observations are consumed later. Isolate transient C++
    # reports per invocation; unrelated captures may have identical run names.
    with TemporaryDirectory(prefix="observations_", dir=scratch_root) as directory:
        summary_path = Path(directory) / "run_summary.json"
        execute_trace_graph(_source_command(source, summary_path, window, threads, cpu_service))
        summary = load_json(summary_path)
        if (io_window.start_ns, io_window.end_ns) != (window.start_ns, window.end_ns):
            execute_trace_graph(_source_command(source, summary_path, io_window, threads, cpu_service))
            summary["source_io_observations"] = load_json(summary_path)["source_io_observations"]
    observation = {
        "source_manifest": str(repo_relative_path(source.manifest_path)),
        "role": role,
        "cpu_service_cost": str(repo_relative_path(cpu_service)) if cpu_service else None,
        "observation_window": {
            "kind": "declared_formal_workload",
            "report": str(repo_relative_path(window.report_path)),
            "start_us": window.start_ns // 1000,
            "end_us": window.end_ns // 1000,
        },
        "io_observation_window": {
            "kind": io_window.source,
            "report": str(repo_relative_path(io_window.report_path)),
            "start_us": io_window.start_ns // 1000,
            "end_us": io_window.end_ns // 1000,
        },
        "source_prefetch_policy": source.hicache_config["prefetch_policy"],
        "source_io_observations": summary["source_io_observations"],
        "source_phase_observations": summary["source_phase_observations"],
    }
    return observation


def _calibration_inputs(group: GroupRequest) -> dict[Path, Path | None]:
    """Bind admitted captures to their CPU correction from the same completed attempt."""
    ledger_path = group.output_dir / "capture_ledger.json"
    inputs = {path.resolve(): None for path in group.fixed_calibration_manifests}
    if ledger_path.exists():
        ledger = load_json(ledger_path)
        if ledger.get("base_config") != group.base_config:
            raise ValueError("calibration output belongs to another base")
        for row in ledger["attempts"]:
            if completed_profile(row) and (row.get("stage") != "profiled_replay" or row.get("cpu_service_cost")):
                manifest = require_repo_path(row["profile_manifest"]).resolve()
                service = row.get("cpu_service_cost")
                inputs[manifest] = require_repo_path(service) if service else None
    if inputs.keys() & {source.manifest_path.resolve() for source in group.sources}:
        raise ValueError("base measurements are not independent calibration")
    return inputs


def scan_group(
    group: GroupRequest,
    *,
    refresh: bool = False,
    cpu_services: tuple[Path, ...] = (),
    build_model: bool = False,
    model_output: Path | None = None,
) -> dict[str, Any]:
    """Observe and select costs once, optionally publishing the ready group model."""

    document_path = group.output_dir / "observations.json"
    previous = load_json(document_path).get("captures", []) if document_path.exists() else []
    cached = {require_repo_path(row["source_manifest"]): row for row in previous}
    base_services = cpu_service_inputs(
        list(cpu_services), tuple(source.manifest_path.resolve() for source in group.sources)
    )
    calibration_services = _calibration_inputs(group)
    calibration_sources = discover_profile_runs(tuple(calibration_services))
    environment = source_environment(group.sources[0]) if calibration_sources else None
    observations = []
    reused = {"base": 0, "calibration": 0}
    for role, sources, services in (
        ("base", group.sources, base_services),
        ("calibration", calibration_sources, calibration_services),
    ):
        for source in sources:
            if role == "calibration" and not environments_match(source_environment(source), environment):
                raise ValueError("calibration changed model, TP or runtime resources")

            service = services.get(source.manifest_path.resolve())
            cached_source = cached.get(source.manifest_path, {})
            reusable = (
                not refresh
                and cached_source.get("role") == role
                and cached_source.get("cpu_service_cost") == (str(repo_relative_path(service)) if service else None)
            )
            if role == "base":
                reusable = (
                    reusable and cached_source.get("source_io_observations", {}).get("cpu_cost_basis") is not None
                )
            if reusable:
                observations.append(cached_source)
                reused[role] += 1
            else:
                observations.append(scan_observations(source, group.output_dir, role=role, cpu_service=service))

    write_json(document_path, {"captures": observations})
    readiness = prepare_model(group, observations, output=(model_output or group.output_dir) if build_model else None)
    readiness.update(base_observations_reused=reused["base"], calibration_observations_reused=reused["calibration"])
    write_json(group.output_dir / "model_inputs.json", readiness)
    return readiness


def main(argv: list[str] | None = None, *, build_model: bool = False) -> int:
    parser = argparse.ArgumentParser(description="Inspect group costs and optionally publish a ready model.")
    parser.add_argument("--group", required=True, type=Path)
    parser.add_argument("--refresh", action="store_true")
    parser.add_argument("--build-model", action="store_true", help="publish the model when cost inputs are ready")
    parser.add_argument("--output-dir", type=Path, help="model output; observations remain in the group directory")
    parser.add_argument("--cpu-service-cost", type=Path, action="append", default=[])
    args = parser.parse_args(argv)
    if not running_in_modeling_container():
        raise SystemExit("use scripts/model.sh prepare-hicache for host orchestration")
    group = GroupRequest.load(require_repo_path(args.group))
    services = args.cpu_service_cost
    if not services:
        plan = cpu_service_plan(group)
        services = prepare_group_cpu_service(plan, dry_run=True)
        if plan["status"] not in {"prepared", "not_requested"}:
            print("CPU inputs need preparation; run scripts/model.sh prepare-hicache --group <group.json>")
            return 2

    result = scan_group(
        group,
        refresh=args.refresh,
        cpu_services=tuple(services),
        build_model=build_model or args.build_model,
        model_output=require_repo_path(args.output_dir) if args.output_dir else None,
    )
    print(f"model_inputs={result['status']} calibration_profiles={result['calibration_profile_count']}")
    return 2 if build_model and result["status"] != "ready" else 0


if __name__ == "__main__":
    raise SystemExit(main())
