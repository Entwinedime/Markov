"""Extract and admit base plus fixed-calibration observations."""

from __future__ import annotations

import time
from pathlib import Path
from typing import Any

from ..common.io import load_json, write_json
from ..common.naming import sanitize
from ..common.paths import repo_relative_path, require_repo_path
from ..modeling.backend import append_option, execute_trace_graph
from ..modeling.cpp_config import trace_graph_executable
from ..modeling.workload import WorkloadWindow, discover_workload_window
from .capture import calibration_inputs, completed_profile
from .coverage import scan_model_inputs
from .fixed_calibration import calibration_page_sizes, materialize_fixed_calibration
from .group import GroupRequest, source_environment
from .planning.profile_runs import ProfileRunDiscovery
from .types import ProfileRunRef


def _source_command(
    source: ProfileRunRef, summary_path: Path, window: WorkloadWindow, threads: int
) -> list[str]:
    command = [
        str(trace_graph_executable({})),
        "--profile-manifest",
        str(source.manifest_path),
        "--run-summary",
        str(summary_path),
        "--trace-channels",
        "torch,ld_preload,python_probe",
        "--threads",
        str(threads),
        "--file-threads",
        str(threads),
    ]
    append_option(command, "--trace-window-start-us", window.start_ns // 1000)
    append_option(command, "--trace-window-end-us", window.end_ns // 1000)
    return command


def _full_request_window(formal: WorkloadWindow) -> WorkloadWindow:
    """Use every controlled request for calibration I/O, not for phase timing."""

    report = load_json(formal.report_path)
    requests = [row for row in report.get("requests", [])
                if isinstance(row, dict) and row.get("kind") == "request"]
    starts = [float(row["start_time_ms"]) for row in requests if row.get("start_time_ms") is not None]
    ends = [float(row["end_time_ms"]) for row in requests if row.get("end_time_ms") is not None]
    if len(starts) != len(requests) or len(ends) != len(requests) or not requests:
        raise ValueError("fixed calibration requires timestamps for every controlled request")
    start_ns = int(min(starts) * 1_000_000)
    end_ns = int(max(ends) * 1_000_000)
    return WorkloadWindow(formal.report_path, start_ns, end_ns, end_ns - start_ns, "all_controlled_requests")


def scan_observations(source: ProfileRunRef, output_dir: Path, *, role: str, threads: int = 4) -> dict[str, Any]:
    """Extract observed work/cost from a source profile without a target config."""

    summary_path = output_dir / "run_summary.json"
    output_dir.mkdir(parents=True, exist_ok=True)
    window = discover_workload_window({}, source.manifest_path)
    if window is None:
        raise ValueError(f"model input has no declared formal workload window: {source.manifest_path}")
    execute_trace_graph(_source_command(source, summary_path, window, threads))
    summary = load_json(summary_path)
    io_window = window
    if role == "calibration":
        io_window = _full_request_window(window)
        io_summary_path = output_dir / "io_run_summary.json"
        execute_trace_graph(_source_command(source, io_summary_path, io_window, threads))
        summary["source_io_observations"] = load_json(io_summary_path)["source_io_observations"]
    return {
        "source_manifest": str(repo_relative_path(source.manifest_path)),
        "summary_path": str(repo_relative_path(summary_path)),
        "role": role,
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


def _fixed_manifests(group: GroupRequest) -> tuple[Path, ...]:
    ledger_path = group.output_dir / "capture_ledger.json"
    manifests = list(group.fixed_calibration_manifests)
    if ledger_path.exists():
        ledger = load_json(ledger_path)
        if ledger.get("base_config") != group.base_config:
            raise ValueError("fixed calibration output belongs to another base")
        expected = calibration_inputs(materialize_fixed_calibration(group)["files"])
        for row in ledger["attempts"]:
            if "calibration_inputs" in row and calibration_inputs(row["calibration_inputs"]) != expected:
                raise ValueError("fixed calibration ledger contains a different fixed endpoint suite")
            if completed_profile(row):
                manifests.append(require_repo_path(row["profile_manifest"]))
    return tuple(dict.fromkeys(manifests))


def _calibration_observations(group: GroupRequest, cached: dict[Path, dict[str, Any]], *, refresh: bool) -> tuple[list[dict], int]:
    manifests = _fixed_manifests(group)
    if not manifests:
        return [], 0
    environment = source_environment(group.sources[0])
    endpoints, _ = calibration_page_sizes(group)
    observations = []
    reused = 0
    for source in ProfileRunDiscovery((), manifests).discover():
        if source_environment(source) != environment:
            raise ValueError("fixed calibration changed model, TP or runtime resources")
        config = source.hicache_config or {}
        if (int(config.get("page_size") or 0) not in endpoints
                or config.get("write_policy") != "write_back"
                or config.get("prefetch_policy") != "wait_complete"):
            raise ValueError("fixed calibration profile does not use a declared endpoint and controlled policy")
        if source.manifest_path in cached and not refresh:
            observations.append(cached[source.manifest_path])
            reused += 1
            continue
        started = time.monotonic()
        observation = scan_observations(
            source,
            group.output_dir / "calibration_observations" / sanitize(source.run_dir.parent.name),
            role="calibration",
        )
        observation["extraction_wall_seconds"] = time.monotonic() - started
        observations.append(observation)
    return observations, reused


def scan_group(group: GroupRequest, *, refresh: bool = False) -> dict[str, Any]:
    """Refresh the fixed model inputs; target declarations are never inspected."""

    document_path = group.output_dir / "observations.json"
    previous = load_json(document_path).get("captures", []) if document_path.exists() else []
    cached = {require_repo_path(row["source_manifest"]): row for row in previous}
    observations = []
    base_reused = 0
    for source in group.sources:
        if source.manifest_path in cached and cached[source.manifest_path].get("role") == "base" and not refresh:
            observations.append(cached[source.manifest_path])
            base_reused += 1
        else:
            observations.append(
                scan_observations(
                    source,
                    group.output_dir / "base_observations" / sanitize(source.input_id),
                    role="base",
                )
            )
    calibration, calibration_reused = _calibration_observations(group, cached, refresh=refresh)
    observations.extend(calibration)
    readiness = scan_model_inputs(group, observations)
    result = {
        "status": "ready_for_model_build" if readiness["status"] == "ready" else "needs_calibration_data",
        "model_inputs": readiness,
        "base_observations_reused": base_reused,
        "calibration_observations_reused": calibration_reused,
        "target_inputs": [],
    }
    write_json(document_path, {"captures": observations})
    write_json(group.output_dir / "calibration_plan.json", result)
    return result


def group_observations(group: GroupRequest) -> list[dict[str, Any]]:
    path = group.output_dir / "observations.json"
    if not path.exists():
        raise ValueError("scan base and fixed calibration observations before model build")
    captures = load_json(path)["captures"]
    base_paths = {source.manifest_path for source in group.sources}
    observed_bases = {require_repo_path(row["source_manifest"]) for row in captures if row["role"] == "base"}
    if observed_bases != base_paths:
        raise ValueError("stored base observations do not match the group request")
    admitted = set(_fixed_manifests(group))
    observed_calibration = {require_repo_path(row["source_manifest"]) for row in captures if row["role"] == "calibration"}
    if observed_calibration != admitted:
        raise ValueError("stored fixed calibration observations do not match the ledger")
    return captures


def main(argv: list[str] | None = None) -> int:
    import argparse
    from ..common.paths import running_in_modeling_container

    parser = argparse.ArgumentParser(description="Extract one base group's fixed model inputs.")
    parser.add_argument("--group", required=True, type=Path)
    parser.add_argument("--refresh", action="store_true")
    args = parser.parse_args(argv)
    if not running_in_modeling_container():
        raise SystemExit("use scripts/model.sh prepare-hicache for host orchestration")
    result = scan_group(GroupRequest.load(require_repo_path(args.group)), refresh=args.refresh)
    print(f"model_inputs={result['model_inputs']['status']} fixed_profiles={result['model_inputs']['fixed_calibration_profile_count']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
