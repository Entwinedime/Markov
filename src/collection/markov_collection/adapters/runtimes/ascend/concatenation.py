"""Declare CANN's dense integer concat dispatch and acquire its local execution prototypes."""

from __future__ import annotations

import platform
from pathlib import Path
from typing import Any

import torch

from markov_collection.adapters.runtimes.pytorch.concatenation import ConcatenationObservation
from markov_collection.adapters.runtimes.pytorch.observation import TorchMarkers


class AscendConcatenation:

    # CANN 9.0 SplitToConcat / IsSupportConcatDV2, commit 51abc745ad50d50ef762d0e843dae2d0cb7ea06b.
    # https://gitcode.com/cann/ops-math/blob/51abc745ad50d50ef762d0e843dae2d0cb7ea06b/conversion/concat_d/op_api/aclnn_cat.cpp
    fan_in = 32
    aligned_fan_in = 512
    alignment_bytes = 32
    dtype_bytes = {"torch.int32": 4, "torch.int64": 8}

    def __init__(self, operators: Any) -> None:
        self.operators = operators
        self.recording = operators.recording
        self.warmed = False
        self.captured = False
        root = Path("/usr/local/Ascend/ascend-toolkit/latest")
        information = root / f"{platform.machine()}-linux/ascend_toolkit_install.info"
        self.version = dict(line.split("=", 1) for line in information.read_text().splitlines())["version"]

    @classmethod
    def points(cls) -> list[tuple[torch.dtype, tuple[int, ...]]]:
        points = []
        for dtype in (torch.int32, torch.int64):
            aligned_extent = cls.alignment_bytes // cls.dtype_bytes[str(dtype)]
            count = cls.fan_in + 1
            for lengths in ((0, 0), (0, 2), (1, 2), (1,) * count, (aligned_extent,) * count):
                points.append((dtype, lengths))
        return points

    def warmup(self) -> None:
        if self.warmed:
            return
        if self.version != "9.0.0" or not torch.npu.get_device_name().startswith("Ascend910B"):
            raise NotImplementedError("integer concat lowering currently qualifies CANN 9.0 on Ascend910B")

        for dtype, lengths in self.points():
            tensors = [torch.ones(length, dtype=dtype, device="npu") for length in lengths]
            torch.cat(tensors)
        torch.npu.synchronize()
        self.warmed = True

    def capture(self) -> None:
        if self.captured:
            return
        for dtype, lengths in self.points():
            for observed in (False, True):
                with self.operators.values.auxiliary():
                    tensors = [torch.ones(length, dtype=dtype, device="npu") for length in lengths]
                identity = len(self.recording.batches)
                self.recording.batches.append(
                    {
                        "id": identity,
                        "kind": "primitive",
                        "phase": "primitive",
                        "members": [],
                        "parameters": {"L": len(lengths), "concat_extent": sum(lengths)},
                        "observation": "source" if observed else "baseline",
                    }
                )
                if observed:
                    self.operators.begin_batch()
                    with self.recording.scope("RuntimePrototype", batch=identity), self.operators:
                        with ConcatenationObservation(self.operators.values):
                            torch.cat(tensors)
                else:
                    with TorchMarkers.scope(f"markov.baseline/{identity}"):
                        torch.cat(tensors)
                torch.npu.synchronize()
        self.captured = True

    @classmethod
    def declare(cls, regions: list[dict[str, Any]], source_values: list[dict[str, Any]]) -> None:
        for region in regions:
            metadata = region["metadata"]
            if metadata.get("interface") != "concatenate":
                continue
            group = metadata["input_groups"]["segments"]
            members = group["members"]
            values = [source_values[member["value"]] for member in members]
            dtype = source_values[metadata["outputs"][0]]["dtype"]
            if (
                group["device"] != "npu"
                or dtype not in cls.dtype_bytes
                or metadata["attributes"]["axis"] != 0
                or any(
                    len(value["shape"]) != 1 or value["stride"] != [1] or value["dtype"] not in cls.dtype_bytes
                    for value in values
                )
            ):
                continue

            # Filtering and alignment are functions of actual members, including a completely full KV pool.
            group["dtype"] = dtype
            types = [value["dtype"] for value in values]
            if any(member != dtype for member in types):
                group["member_dtypes"] = types
            lengths = [member["parameters"]["extent"] for member in members]
            width = cls.dtype_bytes[group["dtype"]]
            nonempty = sum(length > 0 for length in lengths)
            aligned = sum(length > 0 and length * width % cls.alignment_bytes == 0 for length in lengths)
            group["sums"].update(
                {
                    "nonempty_segments": ["lt", 0, "extent"],
                    "aligned_segments": [
                        "and",
                        ["lt", 0, "extent"],
                        ["eq", ["mod", ["mul", width, "extent"], cls.alignment_bytes], 0],
                    ],
                }
            )
            metadata["parameters"].update(nonempty_segments=nonempty, aligned_segments=aligned)
            vectorized = cls.fan_in < nonempty <= cls.aligned_fan_in and aligned == nonempty
            metadata["execution_guards"] = [
                {"condition": ["eq", "nonempty_segments", 0], "expected": nonempty == 0},
                {"condition": ["eq", "nonempty_segments", 1], "expected": nonempty == 1},
                {
                    "condition": [
                        "and",
                        ["lt", cls.fan_in, "nonempty_segments"],
                        ["le", "nonempty_segments", cls.aligned_fan_in],
                        ["eq", "aligned_segments", "nonempty_segments"],
                    ],
                    "expected": vectorized,
                },
            ]
            metadata["entrypoint_phase"] = None

            # Mixed operands keep their native casts; homogeneous reduction prototypes cannot replace them.
            if nonempty > 1 and "member_dtypes" not in group:
                metadata["execution_reduction"] = {
                    "group": "segments",
                    "field": "extent",
                    "fan_in": cls.aligned_fan_in if vectorized else cls.fan_in,
                }
