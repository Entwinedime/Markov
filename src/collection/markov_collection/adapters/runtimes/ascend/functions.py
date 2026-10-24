"""Interfaces for native NPU functions which launch kernels outside torch dispatch."""

from __future__ import annotations

import inspect
from typing import Any

from markov_collection.instrumentation.calls import NativeCall


class SplitQkvCall(NativeCall):
    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        arguments = inspect.signature(self.original).bind(*args, **kwargs).arguments
        shapes = self.values.shapes
        source = arguments["input"]
        if source in shapes.unavailable:
            for output in result:
                shapes.unavailable[output] = self.path
                shapes.tensors.pop(output, None)
                self.recording.values[self.values.value(output)]["shape_relation"] = None
            return f"native function input relation unavailable: {shapes.unavailable[source]}"

        # Q/K/V preserve the symbolic token axis and use widths supplied by the native call.
        rows = shapes.tensor(source).shape[0]
        widths = (arguments["q_hidden_size"], arguments["kv_hidden_size"], arguments["kv_hidden_size"])
        for output, width in zip(result, widths, strict=True):
            self.values.bind_shape(output, (rows, width))

        return None


class AscendFunctionObservation:
    target = "sgl_kernel_npu.norm.split_qkv_rmsnorm_rope.split_qkv_rmsnorm_rope"

    def __init__(self, values: Any) -> None:
        self.values = values

    def split_qkv(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        return SplitQkvCall(
            self.values.recording, self.values, self.target, original, {}, interface="split_qkv_norm_rope"
        )(*args, **kwargs)
