"""Publish a native concatenation with an input group instead of fixed operand slots."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Literal

import torch
from torch.overrides import TorchFunctionMode

from markov_collection.instrumentation.calls import NativeCall


@dataclass(frozen=True)
class GroupRepetition:
    count: str
    kind: Literal["reuse", "produce"]


class ConcatenationCall(NativeCall):
    def __init__(
        self, *args: Any, parent_group: str | None = None, repetition: GroupRepetition | None = None, **kwargs: Any
    ) -> None:
        super().__init__(*args, **kwargs)
        self.parent_group = parent_group
        self.repetition = repetition

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        tensors = args[0]
        axis = (args[1] if len(args) > 1 else kwargs.get("dim", 0)) % result.ndim
        total = sum(tensor.shape[axis] for tensor in tensors)

        metadata["parameters"] = {"L": len(tensors), "concat_extent": total}
        metadata["input_groups"] = {"segments": self.values.input_group(tensors, axis, "L", "concat_extent")}
        if self.parent_group is not None:
            metadata["parent_groups"] = {"segments": self.parent_group}
        if self.repetition is not None:
            metadata["group_repetitions"] = {"segments": {"count": self.repetition.count, "kind": self.repetition.kind}}

        metadata["guards"] = [{"condition": ["eq", "L", 0], "expected": False}]
        metadata["entrypoint"] = "concatenate"
        metadata["argument_relations"] = [{"input_group": "segments"}, *metadata["arguments"][1:]]
        metadata["attributes"] = {"axis": axis}

        # The caller keeps its operand expressions; concat_extent belongs only to this interface.
        shapes = self.values.shapes
        caller_shape = list(shapes.tensor(tensors[0]).shape)
        if self.parent_group is not None:
            region = self.recording.regions[self.recording.current_scope.get()]
            parent = self.recording.regions[region["parent"]]["metadata"]
            sums = parent["input_groups"][self.parent_group]["sums"]
            total_parameter = next(name for name, expression in sums.items() if expression == "extent")
            caller_shape[axis] = shapes.symbol(total_parameter)
        elif self.repetition is not None:
            caller_shape[axis] *= shapes.symbol(self.repetition.count)
        else:
            caller_shape[axis] = sum(shapes.tensor(tensor).shape[axis] for tensor in tensors)
        self.values.bind_shape(result, caller_shape)

        local_shape = shapes.shape(result)
        local_shape[axis] = "concat_extent"
        metadata.setdefault("shape_contracts", []).append({"value": self.values.value(result), "shape": local_shape})
        return None


class ConcatenationObservation(TorchFunctionMode):
    def __init__(self, values: Any, parent_group: str | None = None, repetition: GroupRepetition | None = None) -> None:
        super().__init__()
        self.values = values
        self.scope = values.recording.current_scope.get()
        self.parent_group = parent_group
        self.repetition = repetition

    def __torch_function__(self, function: Any, types: Any, args: tuple = (), kwargs: dict | None = None) -> Any:
        kwargs = kwargs or {}
        if function is not torch.cat or self.values.recording.current_scope.get() != self.scope:
            return function(*args, **kwargs)

        return ConcatenationCall(
            self.values.recording,
            self.values,
            "torch.cat",
            function,
            {},
            interface="concatenate",
            parent_group=self.parent_group,
            repetition=self.repetition,
        )(*args, **kwargs)


class CollectionCall(NativeCall):
    def __init__(self, *args: Any, repetition: GroupRepetition | None = None, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self.repetition = repetition

    def invoke(self, *args: Any, **kwargs: Any) -> Any:
        with ConcatenationObservation(self.values, repetition=self.repetition):
            return super().invoke(*args, **kwargs)
