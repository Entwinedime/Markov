"""Keep data-dependent unique cardinality at an explicit native state boundary."""

from __future__ import annotations

from typing import Any

import torch
from torch.overrides import TorchFunctionMode

from markov_collection.instrumentation.calls import NativeCall


class UniqueCall(NativeCall):
    def __init__(self, values: Any, parameter: str, state: dict[str, Any]) -> None:
        super().__init__(values.recording, values, "torch.unique", torch.unique, {}, interface="tensor_unique")
        self.parameter = parameter
        self.state = state

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        if not isinstance(result, torch.Tensor) or result.ndim != 1:
            return "state cardinality requires a single flat unique result"

        # The owner supplies this cardinality from target state; it is not inferred from input shape.
        self.state.setdefault("parameters", {})[self.parameter] = result.numel()
        self.values.bind_shape(result, (self.values.shapes.symbol(self.parameter),))

        # Keep already-materialized state evidence without copying a device tensor for observation.
        if result.device.type == "cpu":
            metadata["observed_result"] = result.tolist()
        return None


class UniqueObservation(TorchFunctionMode):
    def __init__(self, values: Any, parameter: str) -> None:
        super().__init__()
        region = values.recording.current_scope.get()
        self.call = UniqueCall(values, parameter, values.recording.regions[region]["metadata"])

    def __torch_function__(self, function: Any, types: Any, args: tuple = (), kwargs: dict | None = None) -> Any:
        if function is torch.unique:
            return self.call(*args, **(kwargs or {}))

        return function(*args, **(kwargs or {}))
