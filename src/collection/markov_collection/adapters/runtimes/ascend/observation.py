"""Observe public operations and their native Ascend implementation selection contracts."""

from __future__ import annotations

import sys
from typing import Any

import torch
import torch_npu

from markov_collection.adapters.runtimes.ascend.layouts import AscendLayouts
from markov_collection.adapters.runtimes.pytorch.observation import OperatorObservation


class AscendOperatorObservation(OperatorObservation):
    def record_operation(self, operation: Any, args: tuple, kwargs: dict) -> Any:
        if self.values.interface is not None:
            return super().record_operation(operation, args, kwargs)

        if str(operation) == "aten.floor_divide.default" and not isinstance(args[1], torch.Tensor):
            with self.values.metadata():
                tensor = args[0]
                eligible = (
                    tensor.device.type == "npu"
                    and tensor.ndim == 2
                    and all(size > 0 for size in tensor.shape)
                    and all(stride > 0 for stride in tensor.stride())
                    and torch_npu.get_npu_format(tensor) == 2
                )
                materialization = AscendLayouts.materialization(tensor, self.values.shapes) if eligible else None
            if materialization is None:
                return super().record_operation(operation, args, kwargs)

            # A local call gives serving and component observations the same reusable definition.
            with self.recording.scope("call", path=str(operation)):
                identity = len(self.recording.operations)
                result = super().record_operation(operation, args, kwargs)
                guards, storage = materialization
                self.recording.operations[identity].update(execution_guards=guards, contiguous_storage=storage)
                return result

        if str(operation) not in {"aten.matmul.default", "aten.mm.default"}:
            return super().record_operation(operation, args, kwargs)

        with self.values.metadata():
            left, right = args[:2]
            eligible = (
                left.device.type == right.device.type == "npu"
                and left.ndim == right.ndim == 2
                and left.dtype == right.dtype
                and left.dtype in (torch.bfloat16, torch.float16)
                and tuple(left.stride()) == (left.shape[1], 1)
                and tuple(right.stride()) == (1, right.shape[0])
                and torch_npu.get_npu_format(left) == torch_npu.get_npu_format(right) == 2
            )
            geometry = None
            if eligible:
                relation = self.values.shapes.shape(left)
                geometry = {
                    "rows": left.shape[0],
                    "K": left.shape[1],
                    "N": right.shape[1],
                    "dtype": str(left.dtype),
                    "axis": "matrix_rows",
                }
        if geometry is None:
            return super().record_operation(operation, args, kwargs)

        # Isolate the actual operator, so a local prototype can serve any enclosing model module.
        outer = self.values.shapes.tensors.get(left) if relation is not None else None
        with self.recording.scope("call", path=str(operation)) as scope:
            scope.metadata.update(
                entrypoint="dense_matrix", call_family="dense_matrix", parameters={"matrix_rows": left.shape[0]}
            )
            with self.values.metadata():
                self.values.bounded_parameter("matrix_rows", 1, sys.maxsize)
            with self.values.bind_input(left, {0: "matrix_rows"}):
                with self.values.metadata():
                    scope.metadata["ports"] = {
                        "inputs": self.values.port_values({"left": left, "right": right}), "outputs": {}
                    }
                    scope.metadata["inputs"] = list(scope.metadata["ports"]["inputs"].values())
                    identity = len(self.recording.operations)
                result = super().record_operation(operation, args, kwargs)
                with self.values.metadata():
                    self.recording.operations[identity]["matrix_selection"] = geometry
                    scope.metadata["ports"]["outputs"] = self.values.port_values({"result": result})

        with self.values.metadata():
            if outer is None:
                self.values.shapes.unavailable[result] = "dense matrix input relation unavailable"
                self.values.shapes.tensors.pop(result, None)
            else:
                self.values.bind_shape(result, (outer.shape[0], geometry["N"]))
        return result
