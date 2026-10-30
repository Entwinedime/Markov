"""Measure initialized ATen metadata operations and retain their caller/completion boundaries."""

import argparse
import os
from pathlib import Path

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
    parser.add_argument("--library", type=Path, required=True, help="Matching native ATen sampler library")
    parser.add_argument(
        "--trace",
        action="store_true",
        help="Collect a separate initialized native-port trace after normal API sampling",
    )
    args = parser.parse_args()
    recipe = read_json(args.recipe)
    validate_recipe(recipe)
    library = args.library.resolve(strict=True)
    if os.environ.get("MARKOV_NATIVE_PROBE"):
        raise ValueError("Native tensor calibration must run without the experimental symbol observer")

    args.output.mkdir(parents=True, exist_ok=False)
    import torch
    import torch_npu
    from markov.adapters.ascend.native_tensor_responses import NativeTensorResponses

    torch.set_num_threads(1)
    torch.npu.set_device(recipe["device"])
    native = NativeTensorResponses(library, recipe, args.output)
    native.collect()
    with (args.output / "measurements.jsonl").open("x", encoding="utf-8") as stream:
        native.write_normal_rows(stream)
    if args.trace:
        activities = torch_npu.profiler.ProfilerActivity
        with torch_npu.profiler.profile(
            activities=[activities.CPU, activities.NPU],
            record_shapes=False,
            profile_memory=False,
            with_stack=False,
            experimental_config=torch_npu.profiler._ExperimentalConfig(
                profiler_level=torch_npu.profiler.ProfilerLevel.Level0
            ),
        ) as profiler:
            native.collect(profiled=True)
        profiler.export_chrome_trace(str(args.output / "trace.json"))
    write_json(
        args.output / "capture.json",
        {
            "status": "completed",
            "recipe": recipe,
            "torch": torch.__version__,
            "torch_npu": torch_npu.__version__,
            "device_name": torch.npu.get_device_name(recipe["device"]),
            "native_observer_requested": False,
            "api_boundary": "aten_cpp",
            "process_id": os.getpid(),
            "trace": "trace.json" if args.trace else None,
            "trace_values_verified": args.trace,
            "scope": "Normal C++ ATen API and Linux thread CPU clocks with empty controls; an optional trace is collected in a separate later pass",
            "limitations": "Thread CPU observations include queue housekeeping up to synchronization. Normal enqueue/launch wall ports are not observed. "
            "Profiled partitions locate their estimated shares; device synchronization observation is not bare device service.",
        },
    )


if __name__ == "__main__":
    main()
