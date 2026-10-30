"""Measure initialized ATen metadata operations and retain their caller/completion boundaries."""

import argparse
import json
import os
from pathlib import Path
import time

from markov.artifacts.request import positive_integer, read_json, reject_unknown
from markov.artifacts.writing import write_json


def validate_recipe(recipe):
    reject_unknown(recipe, {"device", "warmup", "repeats", "cases"}, "tensor response recipe")
    if type(recipe["device"]) is not int or recipe["device"] < 0:
        raise ValueError("device must be a nonnegative integer")
    warmup = positive_integer(recipe["warmup"], "warmup")
    repeats = positive_integer(recipe["repeats"], "repeats")
    if not isinstance(recipe["cases"], list) or not recipe["cases"]:
        raise ValueError("cases must be a nonempty array")
    for case in recipe["cases"]:
        reject_unknown(case, {"operation", "dtype", "input_sizes"}, "tensor response case")
        sizes = case["input_sizes"]
        if case["operation"] not in ("clone", "concatenate") or case["dtype"] not in ("int32", "int64"):
            raise ValueError("Supported responses are clone/concatenate with int32/int64")
        if not isinstance(sizes, list) or not sizes or (case["operation"] == "clone" and len(sizes) != 1):
            raise ValueError("input_sizes must describe one clone input or a nonempty concatenate list")
        for size in sizes:
            positive_integer(size, "input size")
    return warmup, repeats


def profile_cases(recipe, output, torch, torch_npu):
    inputs = [
        [torch.arange(size, dtype=getattr(torch, case["dtype"]), device="npu") for size in case["input_sizes"]]
        for case in recipe["cases"]
    ]
    torch.npu.synchronize()
    activities = torch_npu.profiler.ProfilerActivity
    results = []
    with torch_npu.profiler.profile(
        activities=[activities.CPU, activities.NPU],
        record_shapes=False,
        profile_memory=False,
        with_stack=False,
        experimental_config=torch_npu.profiler._ExperimentalConfig(
            profiler_level=torch_npu.profiler.ProfilerLevel.Level0,
        ),
    ) as profiler:
        for index, case in enumerate(recipe["cases"]):
            for ordinal in range(recipe["repeats"]):
                with torch.profiler.record_function(f"markov.tensor:{index}:{ordinal}"):
                    result = inputs[index][0].clone() if case["operation"] == "clone" else torch.cat(inputs[index])
                torch.npu.synchronize()
                results.append((index, result))
    profiler.export_chrome_trace(str(output / "trace.json"))
    for index, result in results:
        expected = torch.cat([tensor.cpu() for tensor in inputs[index]])
        if not torch.equal(result.cpu(), expected):
            raise RuntimeError("Profiled tensor operation did not produce the declared values")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recipe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--trace", action="store_true", help="Collect a separate initialized device trace after normal API sampling"
    )
    args = parser.parse_args()
    recipe = read_json(args.recipe)
    warmup, repeats = validate_recipe(recipe)
    if args.trace and os.environ.get("MARKOV_NATIVE_PROBE"):
        raise ValueError("The tensor trace companion must run without the experimental native observer")

    args.output.mkdir(parents=True, exist_ok=False)
    import torch
    import torch_npu

    torch.set_num_threads(1)
    torch.npu.set_device(recipe["device"])
    with (args.output / "measurements.jsonl").open("x", encoding="utf-8", buffering=1) as stream:
        for case_index, case in enumerate(recipe["cases"]):
            dtype = getattr(torch, case["dtype"])
            sizes = case["input_sizes"]
            inputs = [torch.arange(size, dtype=dtype, device="npu") for size in sizes]
            expected = torch.cat([torch.arange(size, dtype=dtype) for size in sizes])
            for ordinal in range(warmup + repeats):
                torch.npu.synchronize()
                unix_begin = time.time_ns()
                begin, cpu_begin = time.perf_counter_ns(), time.thread_time_ns()
                result = inputs[0].clone() if case["operation"] == "clone" else torch.cat(inputs)
                cpu_ns = time.thread_time_ns() - cpu_begin
                returned = time.perf_counter_ns()
                unix_returned = time.time_ns()
                torch.npu.synchronize()
                completed = time.perf_counter_ns()
                if not torch.equal(result.cpu(), expected):
                    raise RuntimeError("Tensor operation did not produce the declared values")
                stream.write(
                    json.dumps(
                        {
                            "case": case_index,
                            **case,
                            "ordinal": ordinal,
                            "warmup": ordinal < warmup,
                            "caller_unix_begin_ns": unix_begin,
                            "caller_unix_returned_ns": unix_returned,
                            "caller_wall_ns": returned - begin,
                            "caller_thread_cpu_ns": cpu_ns,
                            "completion_observed_ns": completed - begin,
                            "values_verified": True,
                        }
                    )
                    + "\n"
                )
                del result
    if args.trace:
        profile_cases(recipe, args.output, torch, torch_npu)
    write_json(
        args.output / "capture.json",
        {
            "status": "completed",
            "recipe": recipe,
            "torch": torch.__version__,
            "torch_npu": torch_npu.__version__,
            "device_name": torch.npu.get_device_name(recipe["device"]),
            "native_observer_requested": bool(os.environ.get("MARKOV_NATIVE_PROBE")),
            "trace": "trace.json" if args.trace else None,
            "trace_values_verified": args.trace,
            "scope": "Normal API samples run without profiler, Python probe, weights, base or target; an optional trace is collected in a separate later pass",
            "limitations": "Caller response includes enabled native observer overhead; completion includes global synchronization observation. "
            "Neither is bare device service or a decomposition of worker launch/return; native records supply separate boundaries when available.",
        },
    )


if __name__ == "__main__":
    main()
