"""Collect C++ ATen API and thread CPU observations through the runtime's native bridge."""

import csv
import ctypes
import json
from pathlib import Path


class NativeTensorResponses:
    def __init__(self, library: Path, recipe, output: Path):
        self.recipe, self.output = recipe, output
        self.library = ctypes.CDLL(str(library))
        self.sample = self.library.markov_sample_tensor_responses
        self.sample.argtypes = [
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_int64),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        self.sample.restype = ctypes.c_int
        if recipe["warmup"] + recipe["repeats"] > 2**31 - 1:
            raise ValueError("Native repetitions must fit the C++ counter")

    def collect(self, profiled=False):
        for index, case in enumerate(self.recipe["cases"]):
            sizes = (ctypes.c_int64 * len(case["input_sizes"]))(*case["input_sizes"])
            if any(size > (2**63 - 1) // (8 if case["dtype"] == "int64" else 4) for size in case["input_sizes"]):
                raise ValueError("Native tensor input exceeds int64 bytes")
            error = ctypes.create_string_buffer(4096)
            suffix = "profile" if profiled else "normal"
            status = self.sample(
                self.recipe["device"],
                sizes,
                len(sizes),
                8 if case["dtype"] == "int64" else 4,
                int(case["operation"] == "concatenate"),
                0 if profiled else self.recipe["warmup"],
                self.recipe["repeats"],
                index,
                int(profiled),
                str(self.output / f"case_{index}_{suffix}.csv").encode(),
                error,
                len(error),
            )
            if status:
                raise RuntimeError(error.value.decode())

    def write_normal_rows(self, stream):
        for index, case in enumerate(self.recipe["cases"]):
            path = self.output / f"case_{index}_normal.csv"
            threads = {}
            with Path(str(path) + ".threads.csv").open(encoding="utf-8") as source:
                for row in csv.DictReader(source):
                    key = (int(row["ordinal"]), int(row["control"]))
                    threads.setdefault(key, {})[row["tid"]] = int(row["after_ns"]) - int(row["before_ns"])
            with path.open(encoding="utf-8") as source:
                for record in csv.DictReader(source):
                    row = {key: int(value) for key, value in record.items()}
                    row["case"] = index
                    row.update(case)
                    row["warmup"] = bool(row["warmup"])
                    row["control"] = bool(row["control"])
                    row["thread_cpu_ns"] = threads[(row["ordinal"], int(row["control"]))]
                    row["values_verified"] = True
                    stream.write(json.dumps(row) + "\n")
