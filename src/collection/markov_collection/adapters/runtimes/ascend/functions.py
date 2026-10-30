"""Device interfaces beneath public framework coordination."""

from __future__ import annotations

import inspect
from typing import Any

import torch
from torch.overrides import TorchFunctionMode

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


class FlashAttentionCall(NativeCall):
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs, interface="flash_attention")
        self.signature = inspect.signature(self.original)

    def arguments(self, args: tuple[Any, ...], kwargs: dict[str, Any]) -> tuple[Any, Any]:
        arguments = self.signature.bind(*args, **kwargs).arguments
        encoded = self.values.encode(arguments)

        # The caller supplies the selected query extent, not a source-specific launch constant.
        for field in encoded["mapping"]:
            if field[0] == "max_seqlen_q" and field[1] is not None:
                field[1] = {"scalar": "max_query_tokens"}
        return [], encoded

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        arguments = self.signature.bind(*args, **kwargs).arguments

        # The pinned native FA allocates empty_like(q); its strides vary with the query geometry.
        return self.values.shapes.propagate(torch.empty_like, (arguments["q"],), {}, result)


class FusedAttentionCall(NativeCall):
    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        from torch_npu.op_plugin.meta._meta_registrations import npu_fused_infer_attention_score_forward

        # Use the vendor's shape function without converting CPU length tensors into source-specific lists.
        return self.values.shapes.propagate(npu_fused_infer_attention_score_forward, args, kwargs, result)


class AscendFunctionObservation(TorchFunctionMode):
    target = "sgl_kernel_npu.norm.split_qkv_rmsnorm_rope.split_qkv_rmsnorm_rope"

    def __init__(self, values: Any) -> None:
        super().__init__()
        self.values = values

    def split_qkv(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        return SplitQkvCall(
            self.values.recording, self.values, self.target, original, {}, interface="split_qkv_norm_rope"
        )(*args, **kwargs)

    def flash_attention(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        return FlashAttentionCall(self.values.recording, self.values, "flash_attn_with_kvcache", original, {})(
            *args, **kwargs
        )

    def __torch_function__(self, function: Any, types: Any, args: tuple = (), kwargs: dict | None = None) -> Any:
        kwargs = kwargs or {}
        if function is not torch.ops.npu.npu_fused_infer_attention_score:
            return function(*args, **kwargs)

        # Observe the call without replacing the packet that PyTorch uses to resolve overloads.
        return FusedAttentionCall(
            self.values.recording,
            self.values,
            "npu_fused_infer_attention_score",
            function,
            {},
            interface="fused_attention",
        )(*args, **kwargs)
