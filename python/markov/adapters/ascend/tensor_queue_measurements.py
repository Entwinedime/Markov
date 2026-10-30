"""Estimate native CPU ports from normal API/thread clocks and a separate profiled partition."""

import json
from pathlib import Path
from statistics import median

from markov.artifacts.performance import Measurement
from markov.adapters.ascend import tensor_device_measurements, tensor_measurements


SCOPES = (
    "cpu_service",
    "residual_interval",
    "worker_launch_service",
    "worker_launch_residual_interval",
    "worker_return_service",
    "worker_return_residual_interval",
    "dispatch_interval",
    "device_service",
)


def metadata(path):
    report = tensor_device_measurements.metadata(path)
    if report["metadata"].get("api_boundary") != "aten_cpp":
        raise ValueError("Queued CPU estimates require normal C++ ATen measurements, not Python method timings")
    samples = Path(report["capture"]).parent / "measurements.jsonl"
    with samples.open(encoding="utf-8") as stream:
        report["normal_measurements"] = [json.loads(line) for line in stream]
    recipe = report["metadata"]["recipe"]
    rows = report["normal_measurements"]
    keys = {(row["case"], row["control"], row["ordinal"]) for row in rows}
    if len(keys) != len(rows) or len(rows) != len(recipe["cases"]) * (recipe["warmup"] + 2 * recipe["repeats"]):
        raise ValueError("Native sampling rows are duplicated or incomplete")
    for row in rows:
        case = recipe["cases"][row["case"]]
        if any(row[key] != case[key] for key in ("operation", "dtype", "input_sizes")):
            raise ValueError("Native sampling row differs from its declared work")
    return report


def context(report, deployment):
    return {
        **tensor_measurements.context(report["metadata"], deployment),
        "protocol": "paired_native_api_thread_cpu_and_profiled_ports",
    }


def model_kind(case, scope, returned=False):
    operation, implementation, _ = tensor_measurements.kind(case, scope)
    return operation + (".return" if returned else ""), implementation, scope


def recipes(report):
    result = {}
    for case in report["metadata"]["recipe"]["cases"]:
        for scope in SCOPES:
            worker = scope.startswith("worker_") or scope == "dispatch_interval"
            recipe = {
                "feature": "elements" if scope == "device_service" else None,
                "limitations": [
                    "Isolated initialized contiguous 1D operations; no cold start, queue saturation, competing load or allocator pressure coverage.",
                    "Caller CPU and response totals use normal C++ API clocks with empty-timer controls; worker CPU demand subtracts isolated barrier controls.",
                    "CPU port shares are inferred from separately profiled exclusive services, not measured normal enqueue/launch timestamps; worker demand can include queue housekeeping.",
                    "Dispatch and explicit worker residuals retain isolated profiled observations. Their normal-runtime uncertainty is not calibrated.",
                    "CPU uses a typical response per input count in this small measured domain; element count limits coverage, without a fitted CPU slope.",
                ],
            }
            if scope != "device_service" and not (worker and case["operation"] == "clone"):
                recipe["coverage_features"] = ["elements"]
            if case["operation"] == "concatenate":
                recipe["curve"] = "input_tensors"
            result[model_kind(case, scope)] = recipe
            if scope in ("cpu_service", "residual_interval"):
                result[model_kind(case, scope, returned=True)] = recipe
    return result


def measurements(path, report):
    recipe = report["metadata"]["recipe"]
    normal = report["normal_measurements"]
    for case_index, case in enumerate(recipe["cases"]):
        records = [row for row in normal if row["case"] == case_index]
        controls = [row for row in records if row["control"]]
        effective = [row for row in records if not row["control"] and not row["warmup"]]
        if len(controls) != recipe["repeats"] or len(effective) != recipe["repeats"]:
            raise ValueError(f"Missing native normal samples or timer controls for case {case_index}")
        if any(row["values_verified"] is not True or row["pid"] != report["metadata"]["process_id"] for row in records):
            raise ValueError("Normal tensor samples belong to different or unverified execution")
        caller_cpu = median(row["caller_thread_cpu_ns"] for row in effective) - median(
            row["caller_thread_cpu_ns"] for row in controls
        )
        caller_wall = median(row["caller_wall_ns"] for row in effective) - median(
            row["caller_wall_ns"] for row in controls
        )
        if caller_cpu <= 0 or caller_wall <= 0:
            raise ValueError("Normal caller work cannot be distinguished from its clock controls")
        caller_residual = max(0, caller_wall - caller_cpu)
        features = {
            "elements": sum(case["input_sizes"]),
            "input_tensors": len(case["input_sizes"]),
            "element_bytes": 8 if case["dtype"] == "int64" else 4,
            "tensor_rank": 1,
        }
        profiles = [row for row in report["measurements"] if row["case"] == case_index and not row["code_preparation"]]
        for row in profiles:
            parts = row["profiled_cpu"]
            if set(parts) != {"caller_before", "caller_after", "worker_before", "worker_after"}:
                raise ValueError("Native profiling does not provide all caller and worker ports")
            if any(part["wait_ns"] or part["unobserved_ns"] for part in parts.values()):
                raise ValueError("An unexplained wait or missing interval cannot be normalized into CPU service")
            tid = row["worker_tid"]
            if any(tid not in sample["thread_cpu_ns"] for sample in controls + effective):
                raise ValueError("The profiled worker is missing from the same-process normal thread clocks")
            worker_cpu = median(sample["thread_cpu_ns"][tid] for sample in effective) - median(
                sample["thread_cpu_ns"][tid] for sample in controls
            )
            if worker_cpu <= 0:
                raise ValueError("Normal worker demand cannot be distinguished from isolated barrier controls")
            caller_total = parts["caller_before"]["service_ns"] + parts["caller_after"]["service_ns"]
            worker_total = parts["worker_before"]["service_ns"] + parts["worker_after"]["service_ns"]
            if caller_total <= 0 or worker_total <= 0:
                raise ValueError("A profiled partition with no CPU service cannot locate normal CPU ports")
            before = round(caller_cpu * parts["caller_before"]["service_ns"] / caller_total)
            before_gap = round(caller_residual * parts["caller_before"]["service_ns"] / caller_total)
            worker_before = round(worker_cpu * parts["worker_before"]["service_ns"] / worker_total)
            values = {
                ("cpu_service", False): before,
                ("cpu_service", True): round(caller_cpu) - before,
                ("residual_interval", False): before_gap,
                ("residual_interval", True): round(caller_residual) - before_gap,
                ("worker_launch_service", False): worker_before,
                ("worker_return_service", False): round(worker_cpu) - worker_before,
                ("worker_launch_residual_interval", False): parts["worker_before"]["residual_ns"],
                ("worker_return_residual_interval", False): parts["worker_after"]["residual_ns"],
                ("device_service", False): row["device_service_ns"],
            }
            if "dispatch_ns" in row:
                values[("dispatch_interval", False)] = row["dispatch_ns"]
            for (scope, returned), duration in values.items():
                worker = scope.startswith("worker_") or scope == "dispatch_interval"
                work = {
                    key: value
                    for key, value in features.items()
                    if not (worker and case["operation"] == "clone" and key in ("elements", "input_tensors"))
                }
                yield Measurement(
                    row["position"],
                    scope + ("_return" if returned else "") + "_estimate_ns",
                    *model_kind(case, scope, returned),
                    work,
                    duration,
                    False,
                )
