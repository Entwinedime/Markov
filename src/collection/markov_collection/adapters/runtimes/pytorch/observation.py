"""Observe operator interfaces while executing the original native operation."""

from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
from typing import Any

import torch
from torch.multiprocessing.reductions import StorageWeakRef
from torch.utils import _pytree
from torch.utils._python_dispatch import TorchDispatchMode, _disable_current_modes
from torch.utils.weak import WeakIdKeyDictionary

from markov_collection.adapters.runtimes.pytorch.shapes import ShapeFunctions, ShapeRelations
from markov_collection.instrumentation.recording import Recording


class TorchMarkers:
    @staticmethod
    def scope(name: str) -> Any:
        return torch.profiler.record_function(name)


class TensorValues:
    def __init__(self, recording: Recording, shapes: ShapeRelations) -> None:
        self.recording = recording
        self.shapes = shapes
        self.tensors: Any = WeakIdKeyDictionary()
        self.storages: dict[int, tuple[StorageWeakRef, int]] = {}
        self.versions: list[int] = []

        self.interface: int | None = None
        self.tensor_geometry: Any = None

    @contextmanager
    def native_interface(self, identity: int) -> Iterator[None]:
        previous = self.interface
        self.interface = identity
        try:
            yield
        finally:
            self.interface = previous

    @contextmanager
    def metadata(self) -> Iterator[None]:
        with TorchMarkers.scope("markov.observer"), torch._C.DisableTorchFunction(), _disable_current_modes():
            yield

    @contextmanager
    def auxiliary(self) -> Iterator[None]:
        with TorchMarkers.scope("markov.auxiliary"), torch._C.DisableTorchFunction(), _disable_current_modes():
            yield

    def storage(self, tensor: torch.Tensor) -> int:
        storage = tensor.untyped_storage()
        key = storage._cdata
        previous = self.storages.get(key)

        # An allocator may reuse an address; expired storage must receive a fresh logical identity.
        if previous is None or previous[0].expired():
            identity = len(self.versions)
            self.versions.append(0)
            self.storages[key] = (StorageWeakRef(storage), identity)

        return self.storages[key][1]

    def value(self, tensor: torch.Tensor, producer: int | None = None) -> int:
        with torch._C.DisableTorchFunction():
            return self._value(tensor, producer)

    def _value(self, tensor: torch.Tensor, producer: int | None) -> int:
        storage = self.storage(tensor)
        version = self.versions[storage]
        previous = self.tensors.get(tensor)
        if producer is None and previous is not None and previous[0] == (storage, version):
            record = self.recording.values[previous[1]]
            if record["shape_relation"] is None:
                record["shape_relation"] = self.shapes.shape(tensor)
                record["stride_relation"] = self.shapes.strides(tensor)
            return previous[1]

        identity = len(self.recording.values)
        self.recording.values.append(
            {
                "id": identity,
                "storage": storage,
                "version": version,
                "producer": producer,
                "shape": list(tensor.shape),
                "shape_relation": self.shapes.shape(tensor),
                "stride": list(tensor.stride()),
                "stride_relation": self.shapes.strides(tensor),
                "offset": tensor.storage_offset(),
                "dtype": str(tensor.dtype),
                "device": tensor.device.type,
            }
        )
        self.tensors[tensor] = ((storage, version), identity)
        return identity

    def encode(self, value: Any, producer: int | None = None) -> Any:
        if isinstance(value, torch.Tensor):
            return {"value": self.value(value, producer)}
        if isinstance(value, (tuple, list)):
            return [self.encode(item, producer) for item in value]

        # Literal mappings cannot collide with tensor/shape operand tags in the interchange tree.
        if isinstance(value, dict):
            return {"mapping": [[key, self.encode(item, producer)] for key, item in value.items()]}
        if value is None or isinstance(value, (bool, int, float, str)):
            return value
        if isinstance(value, (torch.dtype, torch.device, torch.layout, torch.memory_format)):
            return str(value)

        return {"opaque_type": f"{type(value).__module__}.{type(value).__qualname__}"}

    @staticmethod
    def tensor_leaves(value: Any) -> list[torch.Tensor]:
        return [item for item in _pytree.tree_leaves(value) if isinstance(item, torch.Tensor)]

    def references(self, value: Any) -> list[int]:
        return [self.value(item) for item in self.tensor_leaves(value)]

    def port_values(self, tensors: dict[str, torch.Tensor]) -> dict[str, int]:
        with self.metadata():
            return {name: self.value(tensor) for name, tensor in tensors.items()}

    def input_group(self, tensors: list[torch.Tensor], axis: int, count: str, total: str) -> dict[str, Any]:
        first = tensors[0]
        shape: list[Any] = list(first.shape)
        shape[axis] = "extent"

        return {
            "dtype": str(first.dtype),
            "device": first.device.type,
            "shape": shape,
            "count": count,
            "sums": {total: "extent"},
            "members": [
                {"value": self.value(tensor), "parameters": {"extent": tensor.shape[axis]}} for tensor in tensors
            ],
        }

    def bounded_parameter(self, name: str, minimum: int, maximum: int) -> Any:
        symbol = self.shapes.symbol(name)
        self.shapes.environment.constrain_symbol_range(symbol.node.expr, minimum, maximum)

        # Fake analysis and target selection consume the same declared input range.
        metadata = self.recording.regions[self.recording.current_scope.get()]["metadata"]
        metadata.setdefault("guards", []).append(
            {"condition": ["and", ["le", minimum, name], ["le", name, maximum]], "expected": True}
        )
        return symbol

    def bind(self, tensor: torch.Tensor, axes: dict[int, str]) -> None:
        self.shapes.bind(tensor, axes)
        self.publish_shape(tensor)

    @contextmanager
    def bind_input(self, tensor: torch.Tensor, axes: dict[int, str]) -> Iterator[None]:
        shapes = self.shapes
        previous = shapes.tensors.get(tensor)
        unavailable = shapes.unavailable.get(tensor)

        # A callee's semantic axes must not become the caller's shape environment.
        try:
            self.bind(tensor, axes)
            yield
        finally:
            with self.metadata():
                if previous is None:
                    shapes.tensors.pop(tensor, None)
                else:
                    shapes.tensors[tensor] = previous

                if unavailable is None:
                    shapes.unavailable.pop(tensor, None)
                else:
                    shapes.unavailable[tensor] = unavailable

    def bind_shape(self, tensor: torch.Tensor, shape: Any) -> None:
        self.shapes.bind_shape(tensor, shape)
        self.publish_shape(tensor)

    def publish_shape(self, tensor: torch.Tensor) -> int:
        identity = self.value(tensor)
        shape = self.shapes.shape(tensor)
        if self.recording.values[identity]["shape_relation"] != shape:
            region = self.recording.current_scope.get()
            if region is None:
                raise RuntimeError("an existing value needs a semantic scope for its shape declaration")

            # Preserve the producer's relation; an interface declaration constrains its incoming port.
            self.recording.regions[region]["metadata"].setdefault("shape_contracts", []).append(
                {"value": identity, "shape": shape}
            )

        return identity


class OperatorObservation(TorchDispatchMode):
    def __init__(self, recording: Recording) -> None:
        super().__init__()
        self.recording = recording
        self.values = TensorValues(recording, ShapeRelations())
        self.functions = ShapeFunctions(self.values)

    def __enter__(self) -> OperatorObservation:
        self.functions.__enter__()
        super().__enter__()
        return self

    def __exit__(self, *error: Any) -> Any:
        try:
            return super().__exit__(*error)
        finally:
            self.functions.__exit__(*error)

    def begin_batch(self) -> None:

        # Symbolic assumptions belong to one batch; native storage identities still span batches.
        self.values.shapes = ShapeRelations()
        self.functions.reset()

    def __torch_dispatch__(self, operation: Any, types: Any, args: tuple = (), kwargs: dict | None = None) -> Any:
        kwargs = kwargs or {}
        if self.recording.current_scope.get() is None or operation.namespace == "profiler":
            return operation(*args, **kwargs)

        if operation._schema.name == "aten::_local_scalar_dense" and args[0].device.type != "cpu":
            with self.values.metadata():
                source = self.values.value(args[0])
            with self.recording.scope("HostRead", source=source, path="scalar"):
                return self.record_operation(operation, args, kwargs)

        return self.record_operation(operation, args, kwargs)

    def record_operation(self, operation: Any, args: tuple, kwargs: dict) -> Any:
        identity = len(self.recording.operations)
        record = {"id": identity, "region": self.recording.current_scope.get(), "name": str(operation)}
        self.recording.operations.append(record)

        internal = self.values.interface is not None
        with self.values.metadata():
            if internal:
                record["shape_analysis"] = "interface_internal"
                record["interface_region"] = self.values.interface
            else:
                for value in _pytree.tree_leaves((args, kwargs)):
                    if isinstance(value, torch.Tensor) and value not in self.values.shapes.unavailable:
                        self.values.shapes.tensor(value)

            record["arguments"] = self.values.encode(args)
            record["keywords"] = self.values.encode(kwargs)
            record["inputs"] = self.values.references((args, kwargs))

            # Mutation ownership comes from the operator schema and is lifted to an enclosing interface.
            writes = set()
            for index, parameter in enumerate(operation._schema.arguments):
                if parameter.alias_info is not None and parameter.alias_info.is_write:
                    value = args[index] if index < len(args) else kwargs.get(parameter.name)
                    writes.update(
                        self.values.storage(item)
                        for item in _pytree.tree_leaves(value)
                        if isinstance(item, torch.Tensor)
                    )
            record["writes"] = sorted(writes)
            if internal:
                interface = self.recording.regions[self.values.interface]["metadata"]
                interface["writes"] = sorted(set(interface["writes"]) | writes)

        with TorchMarkers.scope(f"markov.operation/{identity}"):
            result = operation(*args, **kwargs)

        with self.values.metadata():
            geometry = self.functions.scalar_geometry(operation, args, kwargs) or (
                self.functions.geometry
                if self.functions.geometry is not None and operation._schema.name in self.functions.geometry.operations
                else None
            )
            error = None if internal else self.values.shapes.propagate(operation, args, kwargs, result, geometry)
            record["shape_error"] = error
            if geometry is not None:
                record["shape_origin"] = geometry.origin

            # Publish outputs after advancing storage versions so later reads see the completed mutation.
            for storage in writes:
                self.values.versions[storage] += 1
            record["result"] = self.values.encode(result, identity)
            record["outputs"] = self.values.references(result)

            if geometry is not None and not internal and error is None:
                arguments = self.functions.geometry_arguments(
                    operation._schema,
                    record["arguments"],
                    self.values.shapes.shape(result),
                    geometry,
                    self.values.shapes,
                )
                if arguments is not None:
                    record["argument_relations"] = arguments

        return result
