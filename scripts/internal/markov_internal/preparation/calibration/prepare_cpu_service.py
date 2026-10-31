"""Prepare paired CPU service intervals or explicitly convert expanded inputs."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from markov_internal.common.commands import positive_int
from markov_internal.common.io import load_json, write_json
from markov_internal.common.paths import require_repo_path
from markov_internal.modeling.backend import execute_trace_graph, trace_graph_executable
from markov_internal.modeling.workload import controlled_request_window, discover_workload_window
from markov_internal.preparation.calibration.cpu_pair_preparation import prepare_forward_cpu_pair


def convert_cpu_service(source: Path, output_dir: Path) -> dict[str, Any]:
    """Convert expanded intervals explicitly; retain coordinates and provenance."""
    source, output_dir = map(require_repo_path, (source, output_dir))
    output = output_dir / "cpu_service.json"
    if output.exists():
        raise FileExistsError(output)
    raw = load_json(source)
    lanes: dict[tuple[str, str], list[list[int]]] = {}
    for row in raw.pop("spans"):
        lane = row["pid"], row["tid"]
        if any(not isinstance(name, str) or not name for name in lane):
            raise ValueError("CPU correction needs nonempty string thread identities")
        values = [row[key] for key in ("begin_us", "end_us", "service_us")]
        if any(type(value) is not int or not 0 <= value < 2**64 for value in values) or values[0] >= values[1]:
            raise ValueError("CPU correction needs nonempty uint64 intervals")
        lanes.setdefault(lane, []).append(values)

    for spans in lanes.values():
        spans.sort()
        if any(left[1] > right[0] for left, right in zip(spans, spans[1:])):
            raise ValueError("CPU correction intervals overlap")
        previous_end = 0
        for row in spans:
            begin, end, service = row
            row[:] = begin - previous_end, end - begin, service
            previous_end = end
    output_dir.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as stream:
        stream.write(json.dumps(raw, ensure_ascii=False, indent=2).removesuffix("\n}") + ',\n  "lanes": [')
        for index, ((pid, tid), spans) in enumerate(sorted(lanes.items())):
            stream.write(",\n    " if index else "\n    ")
            stream.write(json.dumps(dict(pid=pid, tid=tid, intervals=spans), ensure_ascii=False, separators=(",", ":")))
        stream.write("\n  ]\n}\n")
    return dict(
        status="converted",
        source=str(source),
        output=str(output),
        source_bytes=source.stat().st_size,
        output_bytes=output.stat().st_size,
        lanes=len(lanes),
        intervals=sum(len(spans) for spans in lanes.values()),
        scope="Lossless layout conversion; no pairing, sampling or correction changes",
    )


def prepare_cpu_service(
    light: Path,
    profiled: Path,
    tp_size: int,
    output_dir: Path,
    threads: int = 2,
    correct_recorder: bool = False,
    include_setup_requests: bool = False,
) -> Path:
    """All targets sharing this source reuse the same measured service file."""
    light, profiled, output_dir = map(require_repo_path, (light, profiled, output_dir))
    window = discover_workload_window({}, profiled)
    if window is None or window.source != "workload_report.formal_window":
        raise ValueError("CPU service preparation requires the explicit formal workload window")
    if include_setup_requests:
        window = controlled_request_window(window)
    print("CPU service: reading and pairing the selected base captures", flush=True)
    measurements = prepare_forward_cpu_pair(
        light, profiled, tp_size, correct_recorder=correct_recorder, include_setup_requests=include_setup_requests
    )
    measurements["measurement_window"].update(start_us=window.start_ns // 1000, end_us=window.end_ns // 1000)
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
        if "hook_recorder_correction" in audit:
            measurements["hook_recorder_correction"] = audit["hook_recorder_correction"]
    measurements["preparation"] = dict(
        tp_size=tp_size, correct_recorder=correct_recorder, include_setup_requests=include_setup_requests
    )
    write_json(measurement_path, measurements)
    return service_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--light-manifest", type=Path)
    source.add_argument("--convert-input", type=Path, help="convert expanded CPU intervals without reopening traces")
    parser.add_argument("--profile-manifest", type=Path)
    parser.add_argument("--tp-size", type=positive_int)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--threads", type=positive_int, default=2)
    parser.add_argument(
        "--include-setup-requests",
        action="store_true",
        help="pair every controlled request for independent calibration",
    )
    parser.add_argument(
        "--correct-recorder", action="store_true", help="Apply measured light/source recorder corrections"
    )
    args = parser.parse_args(argv)
    if args.convert_input:
        if args.profile_manifest or args.tp_size or args.correct_recorder or args.include_setup_requests:
            parser.error("--convert-input uses only --output-dir")
        result = convert_cpu_service(args.convert_input, args.output_dir)
        write_json(require_repo_path(args.output_dir) / "conversion_summary.json", result)
        print(f"CPU service converted: {result['output']}")
        return 0
    if args.profile_manifest is None or args.tp_size is None:
        parser.error("CPU pairing requires --profile-manifest and --tp-size")

    path = prepare_cpu_service(
        args.light_manifest,
        args.profile_manifest,
        args.tp_size,
        args.output_dir,
        args.threads,
        args.correct_recorder,
        args.include_setup_requests,
    )
    print(f"CPU service prepared: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
