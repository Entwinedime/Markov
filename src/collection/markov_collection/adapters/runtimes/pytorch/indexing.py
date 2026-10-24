"""Declare indexed tensor geometry and preserve device-to-host coordination."""

from __future__ import annotations

from typing import Any

import torch
from torch.overrides import TorchFunctionMode

from markov_collection.instrumentation.calls import NativeCall


class ScalarReductionObservation(TorchFunctionMode):
    def __init__(self, values: Any, tensor: Any, parameter: str, minimum: int, maximum: int) -> None:
        super().__init__()
        self.values = values
        self.tensor = tensor
        self.parameter = parameter
        self.minimum = minimum
        self.maximum = maximum

    def __torch_function__(self, function: Any, types: Any, args: tuple = (), kwargs: dict | None = None) -> Any:
        kwargs = kwargs or {}
        result = function(*args, **kwargs)
        if function is torch.Tensor.max and len(args) == 1 and args[0] is self.tensor and not kwargs:

            # The caller declares the reduction's meaning; scalar identity preserves it across device readback.
            with self.values.metadata():
                self.values.shapes.scalars[result] = self.values.bounded_parameter(
                    self.parameter, self.minimum, self.maximum
                )
        return result


class TensorAssignmentCall(NativeCall):
    @staticmethod
    def declare(regions: list[dict[str, Any]], values: list[dict[str, Any]]) -> None:
        for region in regions:
            metadata = region["metadata"]
            if metadata.get("interface") != "tensor_assignment":
                continue
            payload = metadata["arguments"][-1]
            if not isinstance(payload, dict) or "value" not in payload:
                continue
            tensor = values[payload["value"]]
            if tensor["shape_relation"] is None:
                continue

            # The native indexing binding removes leading unit axes before dispatching the assignment.
            guards = []
            for size, expression in zip(tensor["shape"], tensor["shape_relation"], strict=True):
                if not isinstance(expression, int):
                    guards.append(
                        {"condition": ["eq", expression, 1], "expected": size == 1, "basis": "assignment_value_rank"}
                    )
                if size != 1:
                    break
            previous = [
                guard for guard in metadata.get("execution_guards", []) if guard.get("basis") != "assignment_value_rank"
            ]
            if guards or previous:
                metadata["execution_guards"] = [*guards, *previous]
            else:
                metadata.pop("execution_guards", None)

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        return self.values.shapes.propagate(self.original, args, kwargs, result)


class TensorViewCall(NativeCall):
    def __init__(self, *args: Any, geometry: tuple[Any, ...], start: Any = None, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self.geometry = geometry
        self.start = start

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        if self.values.storage(result) != self.values.storage(args[0]):
            raise ValueError("declared buffer geometry requires a view of the source")
        if self.start is None and result.storage_offset() != args[0].storage_offset():
            raise ValueError("declared buffer geometry requires a view at the source offset")

        self.values.bind_shape(result, self.geometry)
        metadata["opaque_inputs"] = metadata["inputs"]
        metadata["argument_relations"] = [metadata["arguments"][0], {"shape": self.values.shapes.shape(result)}]
        if self.start is not None:
            metadata["argument_relations"].append({"scalar": self.values.shapes.expression(self.start)})

        return None


class TensorViewObservation(TorchFunctionMode):
    def __init__(
        self, values: Any, buffers: list[tuple[Any, tuple[Any, ...]]], start: Any = None, index: Any = None
    ) -> None:
        super().__init__()
        self.values = values
        self.buffers = {id(tensor): (tensor, shape) for tensor, shape in buffers}
        self.scope = values.recording.current_scope.get()
        self.start = start
        self.index = index
        self.result = None

    def __torch_function__(self, function: Any, types: Any, args: tuple = (), kwargs: dict | None = None) -> Any:
        kwargs = kwargs or {}
        if function is not torch.Tensor.__getitem__ or self.values.recording.current_scope.get() != self.scope:
            return function(*args, **kwargs)

        declared = self.buffers.get(id(args[0]))
        if declared is None or declared[0] is not args[0]:
            return function(*args, **kwargs)
        if self.index is not None:
            if args[1] != self.index:
                return function(*args, **kwargs)
        elif self.start is not None:
            index = args[1]
            if not isinstance(index, slice) or index.stop is not None or index.step not in (None, 1):
                return function(*args, **kwargs)

        self.result = TensorViewCall(
            self.values.recording,
            self.values,
            "torch.Tensor.__getitem__",
            function,
            {},
            interface="buffer_view",
            geometry=declared[1],
            start=self.start,
        )(*args, **kwargs)
        return self.result


class IndexedRegions:
    """Expose device-index reads before their native view and assignment primitives."""

    def __init__(self, regions: list[dict], operations: list[dict], values: list[dict]) -> None:
        self.regions = regions
        self.values = values
        self.children: list[list[int]] = [[] for _ in regions]
        self.operations: list[list[dict]] = [[] for _ in regions]
        for region in regions:
            if region["parent"] is not None:
                self.children[region["parent"]].append(region["id"])
        for operation in operations:
            self.operations[operation["region"]].append(operation)

    def declare(self, identity: int, shape: list, start: Any) -> None:
        descendants = [identity]
        for current in descendants:
            descendants.extend(self.children[current])
        if not any(self.regions[current]["role"] == "HostRead" for current in descendants):
            return

        metadata = self.regions[identity]["metadata"]
        views = [
            operation
            for operation in self.operations[identity]
            if operation["name"] in ("aten.slice.Tensor", "aten.select.int")
        ]
        copies = [operation for operation in self.operations[identity] if operation["name"] == "aten.copy_.default"]
        if len(views) != 1 or len(views) + len(copies) != len(self.operations[identity]) or len(copies) > 1:
            raise NotImplementedError("device-indexed interface needs a native indexing primitive declaration")
        view = views[0]
        result = self.values[view["outputs"][0]]
        original = self.values[view["inputs"][0]]
        metadata.setdefault("parameters", {})["view_offset"] = result["offset"] - original["offset"]
        metadata.setdefault("derived_parameters", []).append({"name": "view_offset", "expression": start})

        # The captured scalar reads stay public; only the following view receives the geometry declaration.
        metadata.pop("interface")
        for current in descendants:
            for operation in self.operations[current]:
                operation.pop("shape_analysis", None)
                operation.pop("interface_region", None)
        view["interface"] = "buffer_view"
        view["argument_relations"] = [view["arguments"][0], {"shape": shape}, {"scalar": "view_offset"}]
        result["shape_relation"] = shape
        for copy in copies:
            for value in copy["outputs"]:
                self.values[value]["shape_relation"] = shape

            # A write also produces the whole-buffer alias returned by the enclosing native call.
            copy["public_outputs"] = metadata["mutated_values"]
