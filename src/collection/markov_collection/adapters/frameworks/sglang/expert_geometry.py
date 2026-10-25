"""Bind native attention/MLP synchronization padding to its actual batch fields."""

from __future__ import annotations

from typing import Any

import torch

from markov_collection.adapters.runtimes.pytorch.indexing import TensorViewObservation
from markov_collection.instrumentation.calls import NativeCall


class ModelPaddingCall(NativeCall):
    def __init__(self, values: Any, original: Any, axis: str) -> None:
        super().__init__(values.recording, values, original.__qualname__, original, {}, interface="model_padding")
        self.axis = axis

    @staticmethod
    def declare(regions: list[dict[str, Any]], values: list[dict[str, Any]], geometries: list[dict[int, Any]]) -> None:
        for region in regions:
            metadata = region["metadata"]
            if metadata.get("interface") != "model_padding":
                continue
            source = metadata["ports"]["inputs"]["tensor"]
            output = metadata["ports"]["outputs"]["tensor"]
            shape = geometries[region["id"]].get(source, values[source]["shape_relation"])
            metadata["guards"] = [
                {
                    "condition": ["eq", shape[0], metadata["argument_relations"][1]["scalar"]],
                    "expected": values[source]["shape"][0] == values[output]["shape"][0],
                }
            ]

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> None:
        shapes = self.values.shapes
        before = shapes.tensor(args[0]).shape
        family = f"padding_{self.axis}"
        metadata.update(
            entrypoint=family,
            entrypoint_phase=None,
            call_family=family,
            ports={
                "inputs": self.values.port_values({"tensor": args[0]}),
                "outputs": self.values.port_values({"tensor": result}),
            },
            argument_relations=[
                {"value": self.values.value(args[0])},
                {"scalar": self.axis},
                kwargs.get("value", args[2] if len(args) > 2 else 0),
            ],
            keywords={"mapping": []},
        )
        self.values.bind_shape(result, (shapes.symbol(self.axis), *before[1:]))


class ModelBufferCall(NativeCall):
    def __init__(self, values: Any, original: Any) -> None:
        super().__init__(values.recording, values, original.__qualname__, original, {}, interface="model_buffer")

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> None:
        self.values.bind_shape(result, (self.values.shapes.symbol("model_tokens"), *result.shape[1:]))
        metadata["argument_relations"] = []
        metadata["keywords"] = {"mapping": []}


class ExpertModelGeometry:
    def __init__(self, values: Any) -> None:
        self.values = values
        self.targets: dict[Any, Any] = {}

    @staticmethod
    def bind_batch(values: Any, tokens: str, members: str, size: int) -> None:
        shapes = values.shapes
        padded = size * ((shapes.symbol(tokens) + size - 1) // size)

        # Keep the derived expression in fake analysis, including divisibility and the nonempty local shard.
        shapes.symbols.update(
            model_tokens=padded,
            model_members=padded if tokens == members else shapes.symbol(members),
        )

    def pad_inputs(self, original: Any, batch: Any, runner: Any, num_tokens: int, bs: int) -> Any:
        if self.values.recording.current_scope.get() is None:
            return original(batch, runner, num_tokens, bs)

        metadata = self.values.recording.regions[self.values.recording.current_scope.get()]["metadata"]
        metadata["parameters"] = {"model_tokens": num_tokens, "model_members": bs}
        metadata["derived_parameters"] = [
            {"name": name, "expression": self.values.shapes.expression(self.values.shapes.symbol(name))}
            for name in ("model_tokens", "model_members")
        ]
        previous = self.targets
        self.targets = {}
        for axis, fields in (
            ("model_tokens", ("input_ids", "positions", "out_cache_loc")),
            (
                "model_members",
                (
                    "req_pool_indices", "seq_lens", "seq_lens_cpu",
                    "extend_seq_lens", "rids_int", "bootstrap_room_ids_int",
                ),
            ),
        ):
            for name in fields:
                tensor = getattr(batch, name)
                if isinstance(tensor, torch.Tensor):
                    self.targets[tensor] = axis
        try:
            return original(batch, runner, num_tokens, bs)
        finally:
            self.targets = previous

    def pad(self, original: Any, batch: Any, tensor: Any, *args: Any, **kwargs: Any) -> Any:
        if tensor not in self.targets:
            return original(batch, tensor, *args, **kwargs)
        return ModelPaddingCall(self.values, original.__get__(batch, type(batch)), self.targets[tensor])(
            tensor, *args, **kwargs
        )

    def buffer(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        if self.values.recording.current_scope.get() is None:
            return original(*args, **kwargs)
        return ModelBufferCall(self.values, original)(*args, **kwargs)

    def unpad(self, original: Any, batch: Any, logits: Any) -> Any:
        if self.values.recording.current_scope.get() is None:
            return original(batch, logits)

        with self.values.metadata():
            shape = (self.values.shapes.symbol("B"), *logits.next_token_logits.shape[1:])

        # The public generation path restores one logits row per real request, after native MLP synchronization.
        with TensorViewObservation(self.values, [(logits.next_token_logits, shape)]):
            return original(batch, logits)

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        targets = {
            "sglang.srt.model_executor.forward_batch_info.ForwardBatch._pad_inputs_to_size": self.pad_inputs,
            "sglang.srt.model_executor.forward_batch_info.ForwardBatch._pad_tensor_to_size": self.pad,
            "sglang.srt.layers.dp_attention.get_local_dp_buffer": self.buffer,
            "sglang.srt.layers.dp_attention.get_global_dp_buffer": self.buffer,
            "sglang.srt.model_executor.forward_batch_info.ForwardBatch.post_forward_mlp_sync_batch": self.unpad,
        }
        for target, observer in targets.items():
            HookRegistry.register(target, observer, HookType.AROUND)
