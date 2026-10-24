"""The common dense attention interface owns output geometry and KV effects."""

from __future__ import annotations

import inspect
from typing import Any

from markov_collection.instrumentation.calls import NativeCall


class AttentionCall(NativeCall):
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs, interface="paged_attention")
        self.layer = self.original.__self__
        self.signature = inspect.signature(self.original)

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
            "q_heads": self.layer.tp_q_head_num,
            "kv_heads": self.layer.tp_k_head_num,
            "qk_dim": self.layer.qk_head_dim,
            "v_dim": self.layer.v_head_dim,
        }
        metadata["bindings"] = {"layer": self.layer.layer_id}

        self.values.bind_shape(result, (rows, self.layer.tp_q_head_num * self.layer.v_head_dim))
        return None
