"""Obtain native dense-matmul choices and share their observed local implementations."""

from __future__ import annotations

import configparser
import json
import platform
import subprocess
from pathlib import Path
from typing import TYPE_CHECKING, Any

import torch
import torch_npu
from torch.utils._python_dispatch import TorchDispatchMode

if TYPE_CHECKING:
    from markov_collection.adapters.runtimes.ascend.observation import AscendOperatorObservation


class AscendMatrices(TorchDispatchMode):
    apis = {"aten.matmul.default", "aten.mm.default", "aten.addmm.default"}

    def __init__(self, operators: AscendOperatorObservation, binary: str, limits: dict[str, int], version: str) -> None:
        super().__init__()
        self.operators = operators
        self.binary = binary
        self.limits = limits
        self.version = version
        self.sources: dict[tuple[str, int, int, str], tuple[Any, torch.Tensor, torch.Tensor | None]] = {}
        self.selections: dict[tuple[str, int, int, str], dict[str, Any]] = {}
        self.captured = False

    @staticmethod
    def key(geometry: dict[str, Any]) -> tuple[str, int, int, str]:
        return geometry["api"], geometry["K"], geometry["N"], geometry["dtype"]

    @staticmethod
    def geometry(operation: Any, args: tuple, kwargs: dict) -> dict[str, Any] | None:
        api = str(operation)
        biased = api == "aten.addmm.default"
        left, right = args[1:3] if biased else args[:2]
        if not (
            left.device.type == right.device.type == "npu"
            and left.ndim == right.ndim == 2
            and left.dtype == right.dtype
            and left.dtype in (torch.bfloat16, torch.float16)
            and tuple(left.stride()) == (left.shape[1], 1)
            and tuple(right.stride()) == (1, right.shape[0])
            and torch_npu.get_npu_format(left) == torch_npu.get_npu_format(right) == 2
        ):
            return None

        if biased:
            bias = args[0]
            if not (
                kwargs.get("alpha", 1) == kwargs.get("beta", 1) == 1
                and bias.device == left.device
                and bias.dtype == left.dtype
                and tuple(bias.shape) == (right.shape[1],)
                and bias.is_contiguous()
                and torch_npu.get_npu_format(bias) == 2
            ):
                return None

        return {
            "api": api,
            "rows": left.shape[0],
            "K": left.shape[1],
            "N": right.shape[1],
            "dtype": str(left.dtype),
            "axis": "matrix_rows",
        }

    def __torch_dispatch__(self, operation: Any, types: Any, args: tuple = (), kwargs: dict | None = None) -> Any:
        kwargs = kwargs or {}
        if str(operation) not in self.apis:
            return operation(*args, **kwargs)

        with self.operators.values.metadata():
            geometry = self.geometry(operation, args, kwargs)
            if geometry is not None:
                biased = geometry["api"] == "aten.addmm.default"
                right = args[2] if biased else args[1]
                self.sources.setdefault(self.key(geometry), (operation, right, args[0] if biased else None))
        return operation(*args, **kwargs)

    def invoke(self, key: tuple[str, int, int, str], left: torch.Tensor) -> Any:
        operation, right, bias = self.sources[key]
        return operation(left, right) if bias is None else operation(bias, left, right)

    def prepare(self) -> None:
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

        descriptions = []

        # The native query exhausts each declared row domain; only compressed conditions are retained.
        for api, inner, columns, dtype in self.sources:
            request = {"api": api, "K": inner, "N": columns, "dtype": dtype, "max_rows": max(self.limits.values())}
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
                    "bias" if api == "aten.addmm.default" else "none",
                ],
                text=True,
            )
            descriptions.append({**request, **json.loads(response)})
        self.selections = {self.key(item): item for item in descriptions}
        self.description = {
            "provider": "LegacyMmCheckHitV3Shape",
            "cann": self.version,
            "soc": soc,
            "cube_cores": cores,
            "matrices": descriptions,
        }

        for key, rows in self.points():
            _, right, _ = self.sources[key]
            self.invoke(key, torch.ones((rows, key[1]), device=right.device, dtype=right.dtype))
        torch.npu.synchronize()

    def points(self) -> list[tuple[tuple[str, int, int, str], int]]:
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
            _, right, _ = self.sources[key]
            with self.operators.values.auxiliary():
                left = torch.ones((rows, key[1]), device=right.device, dtype=right.dtype)
            for observed in (False, True):
                with self.operators.prototype("RuntimePrototype", observed, matrix_rows=rows):
                    if observed:
                        with self.operators.values.metadata():
                            self.operators.values.bounded_parameter("matrix_rows", 1, self.selections[key]["max_rows"])
                            self.operators.values.bind(left, {0: "matrix_rows"})

                    self.invoke(key, left)
                torch.npu.synchronize()
        self.captured = True

    @staticmethod
    def declare(
        regions: list[dict[str, Any]],
        operations: list[dict[str, Any]],
        values: list[dict[str, Any]],
        description: dict[str, Any],
    ) -> None:
        selections = {AscendMatrices.key(item): item for item in description["matrices"]}
        for operation in operations:
            geometry = operation.get("matrix_selection")
            if geometry is None:
                continue

            scope = regions[operation["region"]]
            output = scope["metadata"]["ports"]["outputs"]["result"]
            parent = regions[scope["parent"]]

            # The caller's return relation leaves the local matrix axis inside its own API.
            for contract in parent["metadata"].get("shape_contracts", []):
                if contract["value"] == output:
                    values[output]["shape_relation"] = contract["shape"]
            declarations = scope["metadata"].get("shape_contracts", [])
            scope["metadata"]["shape_contracts"] = [
                declaration for declaration in declarations if declaration["value"] != output
            ] + [{"value": output, "shape": ["matrix_rows", geometry["N"]]}]

            axis = geometry["axis"]
            key = AscendMatrices.key(geometry)
            selection = selections[key]
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
