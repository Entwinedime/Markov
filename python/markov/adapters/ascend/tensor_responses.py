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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recipe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    recipe = read_json(args.recipe)
    warmup, repeats = validate_recipe(recipe)

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
    write_json(
        args.output / "capture.json",
        {
            "status": "completed",
            "recipe": recipe,
            "torch": torch.__version__,
            "torch_npu": torch_npu.__version__,
            "device_name": torch.npu.get_device_name(recipe["device"]),
            "native_observer_requested": bool(os.environ.get("MARKOV_NATIVE_PROBE")),
            "scope": "Initialized isolated ATen API responses; no profiler, Python probe, weights, base or target",
            "limitations": "Caller response includes enabled native observer overhead; completion includes global synchronization observation. "
            "Neither is bare device service or a decomposition of worker launch/return; native records supply separate boundaries when available.",
        },
    )


if __name__ == "__main__":
    main()
