"""Keep attention coordination and device calls inside one semantic region."""

from __future__ import annotations

import inspect
import sys
from typing import Any

from markov_collection.adapters.frameworks.sglang.batches import BatchTensors
from markov_collection.adapters.runtimes.ascend.functions import AscendFunctionObservation
from markov_collection.adapters.runtimes.pytorch.concatenation import ConcatenationObservation
from markov_collection.adapters.runtimes.pytorch.factories import TensorGeometryObservation
from markov_collection.instrumentation.calls import NativeCall


class AttentionCall(NativeCall):
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self.layer = self.original.__self__
        self.signature = inspect.signature(self.original)

    @staticmethod
    def bind_cache(values: Any, pool: Any) -> None:
        pages = values.bounded_parameter("kv_pages", 1, sys.maxsize)

        # Ascend reserves page zero; changing pool capacity also changes each layer's contiguous stride.
        for buffer in (pool.k_buffer, pool.v_buffer):
            values.shapes.bind_contiguous(buffer, (buffer.shape[0], pages + 1, *buffer.shape[2:]))

    def invoke(self, *args: Any, **kwargs: Any) -> Any:
        from sglang.srt.hardware_backend.npu.attention.ascend_backend import AscendAttnBackend
        from sglang.srt.model_executor.forward_context import get_attn_backend

        with self.values.metadata():
            arguments = self.signature.bind(*args, **kwargs).arguments
            shapes = self.values.shapes
            backend = get_attn_backend()
            pool = backend.token_to_kv_pool
            cache = shapes.tensor(pool.k_buffer).shape
            code = AscendAttnBackend.forward_decode.__code__

            # Native Decode rebinds q and num_tokens; retain their input geometry at the call boundary.
            dimensions = {
                (code, "num_tokens"): shapes.tensor(arguments["q"]).shape[0],
                (code, "forward_batch.batch_size"): shapes.tensor(arguments["forward_batch"].seq_lens).shape[0],
                (AscendAttnBackend.forward_mixed.__code__, "num_block"): cache[-4],
                (AscendAttnBackend.forward_mixed.__code__, "block_size"): cache[-3],
            }
            for method in (AscendAttnBackend.forward_extend, AscendAttnBackend.forward_decode):
                dimensions[method.__code__, "self.page_size"] = backend.page_size

            # The native KV writer owns its page/head layout; read those fields without calling the writer again.
            writer = inspect.unwrap(pool.set_kv_buffer).__code__
            for name in ("page_size", "head_num", "head_dim", "v_head_dim"):
                dimensions[writer, f"self.{name}"] = getattr(pool, name)

        with (
            TensorGeometryObservation(self.values, scalars=dimensions),
            AscendFunctionObservation(self.values),
            ConcatenationObservation(self.values),
        ):
            return super().invoke(*args, **kwargs)

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        arguments = self.signature.bind(*args, **kwargs).arguments
        query = arguments["q"]
        shapes = self.values.shapes
        if query in shapes.unavailable:
            shapes.unavailable[result] = self.path
            shapes.tensors.pop(result, None)
            return f"attention query relation unavailable: {shapes.unavailable[query]}"

        # The native layer supplies head geometry; only the query's row count varies with the batch.
        rows = shapes.tensor(query).shape[0]
        metadata["attributes"] = {
            "phase": BatchTensors.phase(arguments["forward_batch"]),
            "q_heads": self.layer.tp_q_head_num,
            "kv_heads": self.layer.tp_k_head_num,
            "qk_dim": self.layer.qk_head_dim,
            "v_dim": self.layer.v_head_dim,
        }
        metadata["bindings"] = {"layer": self.layer.layer_id}

        self.values.bind_shape(result, (rows, self.layer.tp_q_head_num * self.layer.v_head_dim))
        return None
