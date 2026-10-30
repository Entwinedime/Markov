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

        # The caller declares the reduction's meaning; scalar identity preserves it across device readback.
        if function is torch.Tensor.max and len(args) == 1 and args[0] is self.tensor and not kwargs:
            with self.values.metadata():
                self.values.shapes.scalars[result] = self.values.bounded_parameter(
                    self.parameter, self.minimum, self.maximum
                )
        return result


class TensorAssignmentCall(NativeCall):
    @staticmethod
    def input_geometries(regions: list[dict[str, Any]]) -> list[dict[int, Any]]:
        geometries: list[dict[int, Any]] = []
        for region in regions:
            parent = region["parent"]
            geometry = dict(geometries[parent]) if parent is not None else {}
            geometry.update((item["value"], item["shape"]) for item in region["metadata"].get("shape_contracts", []))
            geometries.append(geometry)
        return geometries

    @staticmethod
    def declare(regions: list[dict[str, Any]], values: list[dict[str, Any]], geometries: list[dict[int, Any]]) -> None:
        for region in regions:
            metadata = region["metadata"]
            if metadata.get("interface") != "tensor_assignment":
                continue
            payload = metadata["arguments"][-1]
            if not isinstance(payload, dict) or "value" not in payload:
                continue
            tensor = values[payload["value"]]
            shape = geometries[region["id"]].get(payload["value"], tensor["shape_relation"])
            if shape is None:
                continue

            guards = []
            if any(not isinstance(expression, int) for expression in shape):

                # An empty payload writes no elements; its observed no-op must not match nonempty copies.
                guards.append(
                    {
                        "condition": ["or", *[["eq", expression, 0] for expression in shape]],
                        "expected": 0 in tensor["shape"],
                        "basis": "assignment_empty_payload",
                    }
                )

            # The native indexing binding removes leading unit axes before dispatching the assignment.
            for size, expression in zip(tensor["shape"], shape, strict=True):
                if not isinstance(expression, int):
                    guards.append(
                        {"condition": ["eq", expression, 1], "expected": size == 1, "basis": "assignment_value_rank"}
                    )
                if size != 1:
                    break
            previous = [
                guard
                for guard in metadata.get("execution_guards", [])
                if guard.get("basis") not in {"assignment_value_rank", "assignment_empty_payload"}
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


class TensorPartitionCall(NativeCall):
    def __init__(self, values: Any, original: Any) -> None:
        super().__init__(values.recording, values, original.__qualname__, original, {}, interface="tensor_partition")

    def completed(self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]) -> None:
        tensor = args[0] if args else kwargs["input"]
        sections = args[1] if len(args) > 1 else kwargs["indices_or_sections"]
        dimension = args[2] if len(args) > 2 else kwargs.get("dim", 0)
        source = self.values.shapes.tensor(tensor)
        dimension %= source.ndim
        extent = source.shape[dimension]
        quotient, remainder = extent // sections, extent % sections

        # Native tensor_split keeps the original storage; only each slice's symbolic bounds change.
        for index, output in enumerate(result):
            shape = list(source.shape)
            shape[dimension] = (extent + sections - index - 1) // sections
            start = index * quotient + torch.sym_min(index, remainder)
            with self.values.shapes.mode:
                offset = source.storage_offset() + start * source.stride(dimension)
                fake = source.as_strided(shape, source.stride(), offset)
            self.values.shapes.tensors[output] = fake
            self.values.shapes.unavailable.pop(output, None)
            self.values.publish_shape(output)


class TensorViewObservation(TorchFunctionMode):
    @staticmethod
    def declare_slice(values: Any, view: Any, begin: Any, end: Any) -> None:
        identity = values.value(view)
        producer = values.recording.values[identity]["producer"]
        operation = values.recording.operations[producer]
        if operation["name"] != "aten.slice.Tensor":
            raise ValueError("declared slice bounds require their actual native slice producer")

        # The published view identifies this operation; the caller supplies the slice's semantic range.
        arguments = list(operation["arguments"])
        arguments[2:4] = [{"scalar": begin}, {"scalar": end}]
        operation["argument_relations"] = arguments

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

    def buffer_geometry(self, tensor: Any) -> tuple | None:
        declared = self.buffers.get(id(tensor))
        return declared[1] if declared is not None and declared[0] is tensor else None

    def __torch_function__(self, function: Any, types: Any, args: tuple = (), kwargs: dict | None = None) -> Any:
        kwargs = kwargs or {}
        if function is not torch.Tensor.__getitem__ or self.values.recording.current_scope.get() != self.scope:
            return function(*args, **kwargs)

        geometry = self.buffer_geometry(args[0])
        if geometry is None:
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
            geometry=geometry,
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
