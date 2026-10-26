"""Bind tensor creation to native data provenance and declared factory geometry."""

from __future__ import annotations

import inspect
from typing import Any

import torch

from markov_collection.instrumentation.calls import NativeCall


class TensorGeometryObservation:
    def __init__(
        self,
        values: Any,
        data_shape: tuple[str | int, ...] | None = None,
        sources: tuple[Any, ...] | None = None,
        factories: dict[Any, tuple[str | int | torch.SymInt, ...]] | None = None,
        scalars: dict[tuple[Any, str], int] | None = None,
    ) -> None:
        self.values = values
        self.data_shape = data_shape
        self.factories = {inspect.unwrap(function): shape for function, shape in (factories or {}).items()}
        self.scalars = scalars or {}

        self.sources = None if sources is None else list(sources)
        self.scope = values.recording.current_scope.get()

    def __enter__(self) -> TensorGeometryObservation:
        self.previous = self.values.tensor_geometry
        self.values.tensor_geometry = self
        return self

    def __exit__(self, *error: Any) -> None:
        self.values.tensor_geometry = self.previous

    def bind(self, data: Any) -> None:
        if self.sources is not None:
            self.sources.append(data)

    def data_geometry(self, data: Any) -> tuple[str | int, ...] | None:

        # Source containers and the owning call provide provenance; equal lengths do not.
        if self.values.recording.current_scope.get() != self.scope:
            return None
        if self.sources is None or any(source is data for source in self.sources):
            return self.data_shape

        return None

    def factory_geometry(self, function: Any) -> tuple[str | int | torch.SymInt, ...] | None:
        if self.values.recording.current_scope.get() == self.scope:
            return self.factories.get(function)
        return None

    def local_geometry(self, code: Any, name: str) -> int | None:
        return self.scalars.get((code, name))


class TensorGeometryCall(NativeCall):
    def __init__(
        self,
        *args: Any,
        data_shape: tuple[str | int, ...] | None = None,
        factories: dict[Any, tuple[str | int | torch.SymInt, ...]] | None = None,
        **kwargs: Any,
    ) -> None:
        super().__init__(*args, **kwargs)
        self.data_shape = data_shape
        self.factories = factories

    def invoke(self, *args: Any, **kwargs: Any) -> Any:
        with TensorGeometryObservation(self.values, self.data_shape, factories=self.factories):
            return super().invoke(*args, **kwargs)


class TensorCreationCall(NativeCall):
    def __init__(
        self,
        *args: Any,
        geometry: tuple[str | int, ...] | None = None,
        data_argument: str = "data",
        **kwargs: Any,
    ) -> None:
        super().__init__(*args, **kwargs)
        self.geometry = geometry
        self.data_argument = data_argument

    def arguments(self, args: tuple[Any, ...], kwargs: dict[str, Any]) -> tuple[Any, Any]:
        data = args[0] if args else kwargs[self.data_argument]

        # The constructor's geometry is needed; prompt contents and native pointer arrays are not.
        operand = self.values.encode(data) if isinstance(data, torch.Tensor) else {"host_data": type(data).__name__}
        return [operand, *self.values.encode(args[1:])], self.values.encode(
            {name: value for name, value in kwargs.items() if name != self.data_argument}
        )

    def invoke(self, *args: Any, **kwargs: Any) -> Any:
        self.value_begin = len(self.recording.values)
        return super().invoke(*args, **kwargs)

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:

        # Constructor-owned intermediates stay private while any input storage retains its provenance.
        inputs = {self.recording.values[identity]["storage"] for identity in metadata["inputs"]}
        metadata["private_storages"] = sorted(
            {value["storage"] for value in self.recording.values[self.value_begin :]} - inputs
        )

        data = args[0] if args else kwargs[self.data_argument]
        if self.geometry is not None:
            shape = tuple(
                self.values.shapes.symbol(value) if isinstance(value, str) else value for value in self.geometry
            )
            self.values.bind_shape(result, shape)
            return None

        if isinstance(data, torch.Tensor):
            return self.values.shapes.propagate(self.original, args, kwargs, result)

        self.values.bind_shape(result, tuple(result.shape))
        return None
