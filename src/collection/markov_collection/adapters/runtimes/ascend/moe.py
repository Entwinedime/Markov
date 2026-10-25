"""Declare row geometry for native MoE permutation APIs with data-dependent indices."""

from __future__ import annotations

from typing import Any

from markov_collection.instrumentation.calls import NativeCall


class ExpertPermutationCall(NativeCall):
    def __init__(self, values: Any, original: Any, received_rows: str) -> None:
        super().__init__(
            values.recording, values, original.__qualname__, original, {}, interface=original.__name__
        )
        self.kind = original.__name__
        self.received_rows = received_rows

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> None:
        shapes = self.values.shapes
        tensor = args[0] if args else kwargs["permuted_tokens" if self.kind == "npu_moe_token_unpermute" else "tokens"]
        shape = shapes.tensor(tensor).shape
        if self.kind == "repeat_interleave":

            # Counts come from the routing layout, while the repeated IDs retain their native provenance.
            self.values.bind_shape(result, (shapes.symbol(self.received_rows),))
        elif self.kind == "npu_moe_token_permute":
            indices = args[1] if len(args) > 1 else kwargs["indices"]
            rows = shapes.tensor(indices).numel()
            self.values.bind_shape(result[0], (rows, shape[-1]))
            self.values.bind_shape(result[1], (rows,))
            if "num_out_tokens" in kwargs:
                keywords = self.values.encode(
                    {name: value for name, value in kwargs.items() if name != "num_out_tokens"}
                )
                keywords["mapping"].append(["num_out_tokens", {"scalar": shapes.expression(rows)}])
                metadata["keywords"] = keywords
        else:
            probabilities = args[2] if len(args) > 2 else kwargs.get("probs")
            rows = shape[0] if probabilities is None else shapes.tensor(probabilities).shape[0]
            self.values.bind_shape(result, (rows, shape[-1]))
            if "restore_shape" in kwargs:
                keywords = self.values.encode(
                    {name: value for name, value in kwargs.items() if name != "restore_shape"}
                )
                keywords["mapping"].append(["restore_shape", {"shape": shapes.shape(result)}])
                metadata["keywords"] = keywords
