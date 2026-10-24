"""Obtain native dense-matmul choices and share their observed local implementations."""

from __future__ import annotations

import configparser
import json
import platform
import subprocess
from pathlib import Path
from typing import Any

import torch

from markov_collection.adapters.runtimes.ascend.observation import AscendOperatorObservation
from markov_collection.adapters.runtimes.pytorch.observation import TorchMarkers


class AscendMatrices:
    def __init__(self, operators: AscendOperatorObservation, binary: str, limits: dict[str, int], version: str) -> None:
        self.operators = operators
        self.recording = operators.recording
        self.binary = binary
        self.limits = limits
        self.version = version
        self.sources: dict[tuple[int, int, str, str], torch.Tensor] = {}
        self.selections: dict[tuple[int, int, str, str], dict[str, Any]] = {}
        self.captured = False

    @staticmethod
    def key(inner: int, columns: int, dtype: str, axis: str) -> tuple[int, int, str, str]:
        return inner, columns, dtype, axis

    def prepare(self, weights: list[tuple[torch.Tensor, str]]) -> None:
        soc = torch.npu.get_device_name()
        if self.version != "9.0.0" or not soc.startswith("Ascend910B"):
            raise NotImplementedError("dense matrix selection currently uses the CANN 9.0 Ascend910B metadata ABI")

        root = Path("/usr/local/Ascend/ascend-toolkit/latest")
        configuration = configparser.ConfigParser()
        configuration.read(root / f"{platform.machine()}-linux/data/platform_config/{soc}.ini")
        cores = configuration.getint("SoCInfo", "cube_core_cnt")
        library = (
            root / f"opp/built-in/op_impl/ai_core/tbe/op_host/lib/linux/{platform.machine()}/libophost_comm_legacy.so"
        )

        requests = []
        for weight, axis in weights:
            columns, inner = weight.shape
            key = self.key(inner, columns, str(weight.dtype), axis)
            if key in self.sources:
                continue
            self.sources[key] = weight
            requests.append(
                {"K": inner, "N": columns, "dtype": str(weight.dtype), "axis": axis, "max_rows": self.limits[axis]}
            )

        descriptions = []

        # The native query exhausts each declared row domain; only compressed conditions are retained.
        for request in requests:
            response = subprocess.check_output(
                [
                    self.binary,
                    str(library),
                    str(cores),
                    soc,
                    str(request["K"]),
                    str(request["N"]),
                    request["dtype"],
                    str(request["max_rows"]),
                ],
                text=True,
            )
            descriptions.append({**request, **json.loads(response)})
        self.selections = {self.key(item["K"], item["N"], item["dtype"], item["axis"]): item for item in descriptions}
        self.description = {
            "provider": "LegacyMmCheckHitV3Shape",
            "cann": self.version,
            "soc": soc,
            "cube_cores": cores,
            "matrices": descriptions,
        }

        for key, rows in self.points():
            weight = self.sources[key]
            torch.matmul(torch.ones((rows, key[0]), device=weight.device, dtype=weight.dtype), weight.T)
        torch.npu.synchronize()

    def points(self) -> list[tuple[tuple[int, int, str, str], int]]:
        points = []
        for key, selection in self.selections.items():
            intervals = selection["v3_intervals"]
            if intervals:
                points.append((key, intervals[0][0]))
            first_v2 = 1
            for first, last in intervals:
                if first_v2 < first:
                    break
                first_v2 = last + 1
            if first_v2 <= selection["max_rows"]:
                points.append((key, first_v2))
        return points

    def capture(self) -> None:
        if self.captured:
            return
        for key, rows in self.points():
            weight = self.sources[key]
            with self.operators.values.auxiliary():
                left = torch.ones((rows, key[0]), device=weight.device, dtype=weight.dtype)
                right = weight.T
            for observed in (False, True):
                identity = len(self.recording.batches)
                self.recording.batches.append(
                    {
                        "id": identity,
                        "kind": "primitive",
                        "phase": "primitive",
                        "members": [],
                        "parameters": {key[3]: rows},
                        "observation": "source" if observed else "baseline",
                    }
                )
                if observed:
                    self.operators.begin_batch()
                    with self.recording.scope("RuntimePrototype", batch=identity), self.operators:
                        with self.operators.values.metadata():
                            self.operators.values.bounded_parameter(key[3], 1, self.limits[key[3]])
                            self.operators.values.bind(left, {0: key[3]})
                        torch.matmul(left, right)
                else:
                    with TorchMarkers.scope(f"markov.baseline/{identity}"):
                        torch.matmul(left, right)
                torch.npu.synchronize()
        self.captured = True

    def declare(self) -> None:
        for operation in self.recording.operations:
            geometry = operation.get("matrix_selection")
            if geometry is None:
                continue
            axis = geometry["axis"]
            if not isinstance(axis, str) or axis not in self.limits:
                raise NotImplementedError(f"dense matrix row relation needs a bounded input axis: {axis}")
            key = self.key(geometry["K"], geometry["N"], geometry["dtype"], axis)
            selection = self.selections[key]
            intervals = selection["v3_intervals"]
            selected = any(first <= geometry["rows"] <= last for first, last in intervals)

            # Range and branch use the same expression contract consumed by offline C++ templates.
            operation["execution_guards"] = [
                {"condition": ["and", ["le", 1, axis], ["le", axis, selection["max_rows"]]], "expected": True},
                {
                    "condition": [
                        "or",
                        *(["and", ["le", first, axis], ["le", axis, last]] for first, last in intervals),
                    ]
                    if intervals
                    else 0,
                    "expected": selected,
                },
            ]
