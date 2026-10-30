"""Extract source measurements for explicit offline database admission."""

from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass
import gzip
import json
import shutil
import time
from tempfile import TemporaryDirectory
from pathlib import Path
from typing import Any

from ...common.io import iter_json_lines, load_json
from ...common.paths import repo_relative_path, require_repo_path
from ..backend import append_option, execute_trace_graph, trace_graph_executable
from ..workload import WorkloadWindow, controlled_request_window
from .profiles import ProfileRunRef, cpu_service_metadata


@dataclass(frozen=True)
class SourceMeasurements:
    """Metadata and a row stream whose owner keeps its file alive during import."""

    metadata: dict[str, Any]
    rows_path: Path
    has_header: bool = False
    preparation_wall_seconds: float = 0.0

    def rows(self) -> Iterator[dict[str, Any]]:
        rows = iter_json_lines(self.rows_path)
        if self.has_header:
            next(rows)
        count = 0
        for row in rows:
            count += 1
            yield row
        if count != self.metadata["operator_observation_count"]:
            raise ValueError(f"Incomplete operator measurement stream: {self.rows_path}")

    def retain(self, output: Path) -> Path:
        """Keep measurements and their context in one atomically published gzip file."""

        output.parent.mkdir(parents=True, exist_ok=True)
        if self.rows_path.resolve() == output.resolve():
            return output

        pending = output.with_name(output.name + ".pending")
        opener = gzip.open if self.rows_path.suffix == ".gz" else open
        try:
            with opener(self.rows_path, "rb") as source, gzip.open(pending, "wb") as destination:
                destination.write((json.dumps(self.metadata, ensure_ascii=False) + "\n").encode())
                if self.has_header:
                    source.readline()
                shutil.copyfileobj(source, destination)
            pending.replace(output)
        finally:
            pending.unlink(missing_ok=True)
        return output


def _source_command(
    source: ProfileRunRef,
    summary_path: Path,
    observations_path: Path,
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
        "--operator-observations",
        str(observations_path),
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


@contextmanager
def scan_observations(
    source: ProfileRunRef,
    scratch_root: Path,
    *,
    role: str,
    threads: int = 4,
    cpu_service: Path | None = None,
    include_setup_requests: bool = False,
) -> Iterator[SourceMeasurements]:
    """Extract observed work/cost from a source profile without a target config."""

    started = time.monotonic()
    scratch_root.mkdir(parents=True, exist_ok=True)
    window = source.workload_window
    if window is None:
        raise ValueError(f"model input has no declared formal workload window: {source.manifest_path}")
    if include_setup_requests:
        window = controlled_request_window(window)
    manifest = load_json(source.manifest_path)
    if retained := manifest.get("performance_measurements"):
        path = require_repo_path(retained)
        with gzip.open(path, "rt", encoding="utf-8") as retained_stream:
            metadata = json.loads(retained_stream.readline())
        recorded_cpu = metadata["cpu_service_cost"]
        expected_cpu = cpu_service.resolve() if cpu_service else None
        if require_repo_path(metadata["source_manifest"]).resolve() != source.manifest_path.resolve():
            raise ValueError("Retained measurements belong to another capture")
        if (
            cpu_service is not None
            and (require_repo_path(recorded_cpu).resolve() if recorded_cpu else None) != expected_cpu
        ):
            raise ValueError("Retained measurements use another CPU correction; raw traces require a new capture")
        if (
            metadata["observation_window"]["start_us"] != window.start_ns // 1000
            or metadata["observation_window"]["end_us"] != window.end_ns // 1000
        ):
            raise ValueError("Retained measurement window differs from the requested window")
        yield SourceMeasurements(metadata, path, has_header=True, preparation_wall_seconds=time.monotonic() - started)
        return

    correction = cpu_service_metadata(cpu_service) if cpu_service is not None else None
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

    with TemporaryDirectory(prefix="observations_", dir=scratch_root) as directory:
        summary_path = Path(directory) / "run_summary.json"
        observations_path = Path(directory) / "operator_observations.jsonl"
        execute_trace_graph(_source_command(source, summary_path, observations_path, window, threads, cpu_service))
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
            "source_io_observations": summary["source_io_observations"],
            "operator_observation_count": summary["operator_observation_count"],
        }
        if correction is not None:
            observation["measurement_manifests"] = [observation["source_manifest"], correction["light_manifest"]]
            if "reference_io" in correction:
                observation["reference_io"] = correction["reference_io"]
        else:
            observation["measurement_manifests"] = [observation["source_manifest"]]
        yield SourceMeasurements(observation, observations_path, preparation_wall_seconds=time.monotonic() - started)
