"""Prepare one source's CPU service intervals from its paired base captures."""

from __future__ import annotations

import argparse
from pathlib import Path

from ...common.commands import positive_int
from ...common.io import load_json, write_json
from ...common.paths import require_repo_path
from ...modeling.backend import execute_trace_graph, trace_graph_executable
from ...modeling.workload import discover_workload_window
from .cpu_pair_preparation import prepare_forward_cpu_pair


def prepare_cpu_service(
    light: Path,
    profiled: Path,
    tp_size: int,
    output_dir: Path,
    threads: int = 2,
    correct_recorder: bool = False,
) -> Path:
    """All targets sharing this source reuse the same measured service file."""
    light, profiled, output_dir = map(require_repo_path, (light, profiled, output_dir))
    window = discover_workload_window({}, profiled)
    if window is None or window.source != "workload_report.formal_window":
        raise ValueError("CPU service preparation requires the explicit formal workload window")
    print("CPU service: reading and pairing the selected base captures", flush=True)
    measurements = prepare_forward_cpu_pair(light, profiled, tp_size, correct_recorder=correct_recorder)
    output_dir.mkdir(parents=True, exist_ok=True)
    measurement_path = output_dir / "cpu_measurements.json"
    service_path = output_dir / "cpu_service.json"
    write_json(measurement_path, measurements)
    command = [
        str(trace_graph_executable()),
        "--profile-manifest",
        str(profiled),
        "--trace-window-start-us",
        str(window.start_ns // 1000),
        "--trace-window-end-us",
        str(window.end_ns // 1000),
        "--threads",
        str(threads),
        "--file-threads",
        str(threads),
        "--prepare-cpu-service",
        str(measurement_path),
        "--cpu-service-output",
        str(service_path),
    ]
    print("CPU service: binding measured costs to the source DAG", flush=True)
    execute_trace_graph(command)
    if correct_recorder:
        audit = load_json(service_path.with_suffix(".retained.json"))
        correction = audit["source_recorder_correction"]
        existing = sum(round(row["measured_service_delta_us"]) for row in measurements["host_measurements"]["rows"])
        correction.update(
            existing_host_reduction_us=existing,
            new_host_reduction_us=existing + correction["added_integer_reduction_us"],
            applied_new_host_reduction_us=existing + correction["applied_added_integer_reduction_us"],
        )
        measurements["source_recorder_correction"] = correction
    measurements["preparation"] = dict(tp_size=tp_size, correct_recorder=correct_recorder)
    write_json(measurement_path, measurements)
    return service_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--light-manifest", type=Path, required=True)
    parser.add_argument("--profile-manifest", type=Path, required=True)
    parser.add_argument("--tp-size", type=positive_int, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--threads", type=positive_int, default=2)
    parser.add_argument(
        "--correct-recorder", action="store_true", help="Apply measured light/source recorder corrections"
    )
    args = parser.parse_args(argv)
    path = prepare_cpu_service(
        args.light_manifest,
        args.profile_manifest,
        args.tp_size,
        args.output_dir,
        args.threads,
        args.correct_recorder,
    )
    print(f"CPU service prepared: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
