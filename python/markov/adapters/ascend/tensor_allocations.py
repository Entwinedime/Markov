"""Measure real ATen allocation responses in an initialized torch_npu runtime."""

import argparse
import ctypes
import json
from pathlib import Path

from markov.artifacts.request import positive_integer, read_json, reject_unknown, resolve_path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recipe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    recipe = read_json(args.recipe)
    reject_unknown(recipe, {"library", "device", "warmup", "repeats", "cases"}, "allocation recipe")
    library = resolve_path(args.recipe.resolve().parent, recipe["library"])
    if not library.is_file():
        raise FileNotFoundError(library)
    device = recipe["device"]
    if isinstance(device, bool) or not isinstance(device, int) or device < 0:
        raise ValueError("device must be a nonnegative integer")
    warmup = positive_integer(recipe["warmup"], "warmup")
    repeats = positive_integer(recipe["repeats"], "repeats")
    if max(warmup, repeats) > 2**31 - 1:
        raise ValueError("warmup and repeats must fit the native int interface")
    if not isinstance(recipe["cases"], list) or not recipe["cases"]:
        raise ValueError("cases must be a nonempty array")
    for case in recipe["cases"]:
        reject_unknown(case, {"api", "elements", "element_bytes"}, "allocation case")
        if (
            case["api"] not in ("empty", "empty_strided")
            or type(case["element_bytes"]) is not int
            or case["element_bytes"] not in (4, 8)
        ):
            raise ValueError("allocation supports empty/empty_strided and int32/int64")
        elements = positive_integer(case["elements"], "elements")
        if elements > (2**63 - 1) // case["element_bytes"]:
            raise ValueError("allocation size exceeds int64 bytes")

    args.output.mkdir(parents=True, exist_ok=False)
    import torch
    import torch_npu  # noqa: F401

    torch.set_num_threads(1)
    torch.npu.set_device(device)
    torch.npu.synchronize()
    bridge = ctypes.CDLL(str(library))
    sample = bridge.markov_sample_tensor_allocations
    sample.argtypes = [
        ctypes.c_int,
        ctypes.c_int64,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_size_t,
    ]
    sample.restype = ctypes.c_int
    for ordinal, case in enumerate(recipe["cases"]):
        error = ctypes.create_string_buffer(4096)
        status = sample(
            device,
            case["elements"],
            case["element_bytes"],
            int(case["api"] == "empty_strided"),
            warmup,
            repeats,
            str(args.output / f"case_{ordinal}.csv").encode(),
            error,
            len(error),
        )
        if status:
            raise RuntimeError(error.value.decode())

    (args.output / "capture.json").write_text(
        json.dumps(
            {
                "scope": "ATen host allocation response; no profiler, probe, weights or target trace",
                "pool_condition": "Idle initialized runtime, repeated allocation; tensor release outside timing",
                "device": device,
                "torch": torch.__version__,
                "warmup_per_case": warmup,
                "repeats_per_case": repeats,
                "cases": recipe["cases"],
                "metadata_verified": True,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
