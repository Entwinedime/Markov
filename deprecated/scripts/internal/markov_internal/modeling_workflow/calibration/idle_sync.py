"""Measure idle-stream synchronization, separately from device waiting and readback."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
from typing import Any

from ...common.commands import positive_int
from ...common.io import write_json
from ...common.paths import ROOT_DIR
from ...common.process import run_command
from .options import format_cpu_set, parse_cpu_sets, parse_integer_csv


def operation_observations(report: dict[str, Any]) -> list[dict[str, Any]]:
    """Pool the two infinite-wait API variants as one idle-service primitive.

    API and rank remain in the measurement position, not fitting coefficients.
    This is a constant idle-runtime approximation, not a wake-up latency model.
    """
    if report["status"] != "measured" or report["stream_state"] != "completed_event_no_pending_work":
        raise ValueError("Idle synchronization requires completed measurements on a previously used stream")

    return [
        {
            "position": f"rank:{scope['rank']}:repeat:{sample['repeat']}:{api}",
            "operation": "runtime.stream_sync",
            "implementation": "ascend.runtime",
            "timing_scope": "idle_stream_sync_interval",
            "features": {"previous_work_completed": 1.0},
            "duration_us": sample[api + "_us"],
        }
        for scope in report["samples"]
        for sample in scope["samples"]
        for api in ("rtStreamSynchronize", "rtStreamSynchronizeWithTimeout")
    ]


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--devices", required=True, help="Explicit idle devices, sampled sequentially")
    parser.add_argument("--cpu-sets", required=True, help="One CPU set per device, separated by |")
    parser.add_argument("--batch", type=positive_int, default=128)
    parser.add_argument("--repeats", type=positive_int, default=7)
    args = parser.parse_args(argv)

    devices = parse_integer_csv(args.devices, "devices", allow_zero=True)
    cpu_sets = parse_cpu_sets(args.cpu_sets, len(devices))
    allowed = os.sched_getaffinity(0)
    if any(not cpus <= allowed for cpus in cpu_sets):
        raise ValueError("Sampling CPUs must belong to the process allowed affinity")
    if os.environ.get("LD_PRELOAD"):
        raise ValueError("Idle synchronization must be measured without LD_PRELOAD instrumentation")

    build = ROOT_DIR / "build/profiling/runtime_calibration"
    run_command(
        [
            "cmake",
            "-S",
            str(ROOT_DIR / "src/profiling/calibration"),
            "-B",
            str(build),
            "-DCMAKE_BUILD_TYPE=Release",
        ],
        log_path=args.output_dir / "configure.log",
    ).check_returncode()
    run_command(
        ["cmake", "--build", str(build), "--target", "idle_stream_sync", "-j2"],
        log_path=args.output_dir / "build.log",
    ).check_returncode()

    report = {
        "status": "sampling",
        "clock": "steady_clock",
        "timing_scope": "idle_stream_sync_interval",
        "stream_state": "completed_event_no_pending_work",
        "batch": args.batch,
        "repeats": args.repeats,
        "target_workload_trace_used": False,
        "target_score_used": False,
        "samples": [],
        "limitations": [
            "Measures libruntime entrypoints used by the SGLang hook, not the enclosing ACL wrappers.",
            "Stream has completed an event; no pending producer work or inference contention.",
            "Wall service includes scheduling noise, not pure thread CPU time.",
            "Batched calls amortize clock reads; no Python, profiler or probe in the timed loop.",
            "Idle service approximates synchronization after dependencies become ready; wake-up latency is unmeasured.",
            "Timeout variant uses -1 (infinite); other timeout policies are not measured.",
        ],
    }
    output = args.output_dir / "idle_sync.json"
    write_json(output, report)
    try:
        for rank, (device, cpus) in enumerate(zip(devices, cpu_sets)):
            result = run_command(
                [
                    "taskset",
                    "-c",
                    format_cpu_set(cpus),
                    str(build / "idle_stream_sync"),
                    str(device),
                    str(args.batch),
                    str(args.repeats),
                ],
                capture_output=True,
            )
            result.check_returncode()
            report["samples"].append(dict(json.loads(result.stdout), rank=rank, cpu_affinity=sorted(cpus)))
            write_json(output, report)

        report["status"] = "measured"
        report["operator_observations"] = operation_observations(report)
    except BaseException as error:
        report.update(status="failed", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        write_json(output, report)


if __name__ == "__main__":
    main()
