"""Extract source measurements for explicit offline database admission."""

from __future__ import annotations

from tempfile import TemporaryDirectory
from pathlib import Path
from typing import Any

from ...common.io import load_json
from ...common.paths import repo_relative_path
from ..backend import append_option, execute_trace_graph, trace_graph_executable
from ..workload import WorkloadWindow, controlled_request_window
from .profiles import ProfileRunRef


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
    source: ProfileRunRef,
    scratch_root: Path,
    *,
    role: str,
    threads: int = 4,
    cpu_service: Path | None = None,
    include_setup_requests: bool = False,
) -> dict[str, Any]:
    """Extract observed work/cost from a source profile without a target config."""

    scratch_root.mkdir(parents=True, exist_ok=True)
    window = source.workload_window
    if window is None:
        raise ValueError(f"model input has no declared formal workload window: {source.manifest_path}")
    if include_setup_requests:
        window = controlled_request_window(window)
    correction = load_json(cpu_service) if cpu_service is not None else None
    if include_setup_requests and correction is not None:
        measured = correction.get("measurement_window")
        if (
            measured is None
            or measured["scope"] != "all_controlled_requests"
            or measured["start_us"] > window.start_ns // 1000
            or measured["end_us"] < window.end_ns // 1000
        ):
            raise ValueError(
                "Setup-request CPU measurements require a paired correction covering all controlled requests"
            )
    # Only extracted observations are consumed later. Isolate transient C++
    # reports per invocation; unrelated captures may have identical run names.
    with TemporaryDirectory(prefix="observations_", dir=scratch_root) as directory:
        summary_path = Path(directory) / "run_summary.json"
        execute_trace_graph(_source_command(source, summary_path, window, threads, cpu_service))
        summary = load_json(summary_path)
    observation = {
        "source_manifest": str(repo_relative_path(source.manifest_path)),
        "role": role,
        "cpu_service_cost": str(repo_relative_path(cpu_service)) if cpu_service else None,
        "observation_window": {
            "kind": window.source,
            "report": str(repo_relative_path(window.report_path)),
            "start_us": window.start_ns // 1000,
            "end_us": window.end_ns // 1000,
        },
        "io_observation_window": {
            "kind": window.source,
            "report": str(repo_relative_path(window.report_path)),
            "start_us": window.start_ns // 1000,
            "end_us": window.end_ns // 1000,
        },
        "source_prefetch_policy": source.hicache_config["prefetch_policy"],
        "source_io_observations": summary["source_io_observations"],
        "source_phase_observations": summary["source_phase_observations"],
        "operator_observations": summary["operator_observations"],
        "operator_geometry": summary["operator_geometry"],
    }
    if correction is not None:
        observation["measurement_manifests"] = [observation["source_manifest"], correction["light_manifest"]]
        if "reference_io" in correction:
            observation["reference_io"] = correction["reference_io"]
    else:
        observation["measurement_manifests"] = [observation["source_manifest"]]

    return observation
