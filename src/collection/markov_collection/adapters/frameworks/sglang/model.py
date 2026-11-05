"""Keep native input preparation separate from the model's complete reusable call."""

from __future__ import annotations

from dataclasses import fields
from typing import Any

import torch

from markov_collection.adapters.frameworks.sglang.pipeline import PipelineObservation
from markov_collection.adapters.runtimes.pytorch.modules import ModuleState
from markov_collection.instrumentation.calls import NativeCall


class ModelForwardCall(NativeCall):
    def __init__(self, *args: Any, state: ModuleState, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self.state = state

    def resources(self, arguments: dict[str, Any]) -> tuple[Any, Any]:
        from sglang.srt.model_executor.forward_batch_info import ForwardBatch, PPProxyTensors
        from sglang.srt.model_executor.forward_context import get_attn_backend

        inputs = dict(self.state.tensors)

        # Model and backend state keep their geometry in this finalized implementation domain.
        for tensor in inputs.values():
            self.values.shapes.tensor(tensor)

        for name, value in arguments.items():
            if isinstance(value, torch.Tensor):
                inputs[name] = value
            elif isinstance(value, ForwardBatch):
                inputs.update(
                    {
                        f"{name}.{field.name}": tensor
                        for field in fields(value)
                        if isinstance(tensor := getattr(value, field.name), torch.Tensor)
                    }
                )
            elif isinstance(value, PPProxyTensors):
                inputs.update(
                    {f"{name}.{key}": tensor for key, tensor in PipelineObservation.payload(value.tensors).items()}
                )

        backend = get_attn_backend()
        pool = backend.token_to_kv_pool
        resources = {"cache.k": pool.k_buffer, "cache.v": pool.v_buffer}

        # SGLang owns these backend objects; the runtime layer does not interpret framework fields.
        inputs.update(resources)
        inputs.update(
            {
                f"attention.{name}": tensor
                for name, tensor in vars(backend.forward_metadata).items()
                if isinstance(tensor, torch.Tensor)
            }
        )
        metadata = self.recording.regions[self.recording.current_scope.get()]["metadata"]
        metadata["ports"] = {"inputs": self.values.port_values(inputs), "outputs": {}}
        return (), ()

    @staticmethod
    def outputs(result: Any) -> dict[str, torch.Tensor]:
        from sglang.srt.model_executor.forward_batch_info import PPProxyTensors

        if isinstance(result, PPProxyTensors):
            return {f"proxy.{name}": tensor for name, tensor in PipelineObservation.payload(result.tensors).items()}
        return {
            field.name: value
            for field in fields(result)
            if isinstance(value := getattr(result, field.name), torch.Tensor)
        }

    def completed(self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]) -> None:
        metadata["ports"]["outputs"] = self.values.port_values(self.outputs(result))
