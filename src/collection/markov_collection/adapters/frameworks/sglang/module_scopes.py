"""Select scopes from the native module hierarchy and declare SGLang's flat-token ABI."""

from __future__ import annotations

import inspect
from collections.abc import Iterator
from dataclasses import fields, is_dataclass
from functools import partial, update_wrapper
from typing import Any

import torch
from markov_collection.adapters.frameworks.sglang.module_layout import LayerCommunication
from sglang.srt.model_executor.forward_batch_info import PPProxyTensors


def tensor_ports(value: Any, path: str) -> Iterator[tuple[str, torch.Tensor]]:
    if isinstance(value, torch.Tensor):
        yield path, value
    elif isinstance(value, PPProxyTensors):
        yield from tensor_ports(value.tensors, path)
    elif is_dataclass(value) and not isinstance(value, type):
        for field in fields(value):
            yield from tensor_ports(getattr(value, field.name), f"{path}.{field.name}")
    elif isinstance(value, dict):
        for name, item in value.items():
            yield from tensor_ports(item, f"{path}.{name}")
    elif isinstance(value, (tuple, list)):
        for index, item in enumerate(value):
            yield from tensor_ports(item, f"{path}.{index}")


class ModelScopes:
    token_arguments = {
        "input_ids", "positions", "input_embeds", "hidden_states", "residual", "post_residual_addition",
        "captured_last_layer_outputs", "pp_proxy_tensors", "input_", "q", "k", "v",
    }

    def __init__(self, observation: Any, model: Any) -> None:
        from sglang.srt.layers.logits_processor import LogitsProcessor
        from sglang.srt.layers.moe.fused_moe_triton.layer import FusedMoE
        from sglang.srt.layers.moe.topk import TopK
        from sglang.srt.layers.radix_attention import RadixAttention
        from sglang.srt.layers.utils.common import PPMissingLayer
        from sglang.srt.layers.vocab_parallel_embedding import ParallelLMHead, VocabParallelEmbedding

        self.observation = observation
        self.model = model
        modules = dict(model.named_modules())
        self.experts = {name: module for name, module in modules.items() if isinstance(module, FusedMoE)}
        self.expert_layers = []
        for path, module in self.experts.items():

            # The enclosing MoE block pairs its routing module with the expert executor; executor top_k is optional.
            router, = [child for child in modules[path.rpartition(".")[0]].children() if isinstance(child, TopK)]
            self.expert_layers.append({
                "layer": module.layer_id, "experts": module._num_global_routed, "top_k": router.topk_config.top_k,
            })
        selected: dict[str, str] = {"": "model_body"}
        containers = [name for name, module in modules.items() if isinstance(module, torch.nn.ModuleList)]
        outer = [name for name in containers if not any(name.startswith(parent + ".") for parent in containers if parent != name)]
        for container in outer:
            for name, layer in modules[container].named_children():
                if isinstance(layer, PPMissingLayer):
                    continue
                path = f"{container}.{name}" if container else name
                selected[path] = "layer"
        for name, module in modules.items():
            if isinstance(module, LogitsProcessor):
                original = module._compute_lm_head
                module._compute_lm_head = update_wrapper(partial(self.logits, original), original)
            if isinstance(module, RadixAttention):
                selected[name] = "attention"
            elif isinstance(module, VocabParallelEmbedding) and not isinstance(module, ParallelLMHead):
                selected[name] = "embedding"

        self.bindings = {}
        for path, role in selected.items():
            module = modules[path]
            original = module.forward
            expert_layers = {child.layer_id for child in module.modules() if isinstance(child, FusedMoE)} if role == "layer" else set()
            if len(expert_layers) > 1:
                raise ValueError("decoder scope contains multiple expert layers with independent routes")
            rows = {"input": "model_tokens", "output": "model_tokens"}
            communicator = LayerCommunication.read(module)
            if role == "layer" and communicator is not None:
                rows = communicator.rows()
            self.bindings[path] = {
                "rows": rows,
                "expert_layer": next(iter(expert_layers), None),
                "role": role, "signature": inspect.signature(original),
                "implementation": f"{type(module).__module__}.{type(module).__qualname__}",
                "weights": [name for name, _ in module.named_parameters(recurse=False)], "module": module,
            }

            # Native capability checks inspect forward's signature after instrumentation is installed.
            module.forward = update_wrapper(partial(self.invoke, path, original), original)

        # A pipeline input enters the first local decoder with that decoder's native token partition.
        first_layer = next(spec for spec in self.bindings.values() if spec["role"] == "layer")
        self.proxy_rows = first_layer["rows"]["input"]

    def logits(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        result = original(*args, **kwargs)
        owner = self.observation
        if owner.active and owner.selected:
            parameters = owner.capture.scopes[owner.capture.current.get()]["parameters"]

            # Serving without input-logprob expansion projects one selected hidden state per model member.
            if result.ndim != 2 or result.shape[0] != parameters["model_members"]:
                raise ValueError("logits projection needs a declared row relation for this native path")
            with owner.tensors.metadata():
                owner.tensors.declare_shape(result, ["model_members", result.shape[1]])
        return result

    def shape(self, spec, name: str, tensor: torch.Tensor, *, output: bool) -> list[Any] | None:
        if not output and name.split(".", 2)[1] not in self.token_arguments:
            return self.observation.tensors.declared_shape(tensor)
        if output and spec["role"] == "model_body":

            # The model wrapper forwards layer/logits results; it cannot replace their producer geometry.
            declared = self.observation.tensors.declared_shape(tensor)
            if declared is not None:
                return declared

        shape: list[Any] = list(tensor.shape)
        if not shape:
            return shape

        # The decoder ABI is flat-token based; logits select one row per request, independently of token count.
        axis = "model_members" if output and ".next_token_" in name else "model_tokens"
        if not output and name.startswith("argument.pp_proxy_tensors."):
            axis = self.proxy_rows
        if spec["role"] == "layer" and (output or name.split(".", 2)[1] in {"hidden_states", "residual", "post_residual_addition"}):
            axis = spec["rows"]["output" if output else "input"]
        shape[0] = axis
        return shape

    def proxy_shape(self, tensor):
        return [self.proxy_rows, *tensor.shape[1:]]

    def invoke(self, path: str, original: Any, *args: Any, **kwargs: Any) -> Any:
        owner = self.observation
        if not owner.active or not owner.selected:
            return original(*args, **kwargs)

        spec = self.bindings[path]
        bound = spec["signature"].bind(*args, **kwargs)
        bound.apply_defaults()
        arguments = bound.arguments
        if spec["role"] == "model_body":
            tokens = arguments["input_ids"]
            if tokens is None:
                tokens = arguments["pp_proxy_tensors"].tensors["hidden_states"]
            owner.model_geometry(tokens.shape[0], arguments["forward_batch"].batch_size)
        inputs = []
        for name, value in arguments.items():
            if name == "forward_batch":
                continue
            inputs.extend(tensor_ports(value, f"argument.{name}"))

        cache = None
        if spec["role"] == "attention":
            from markov_collection.adapters.frameworks.sglang.cache_ports import AttentionCachePorts

            cache = AttentionCachePorts(owner.tensors, spec["module"], arguments)
            if cache.layout != owner.kv_layout:
                raise NotImplementedError("multiple KV pool layouts require separate pool identities")

        scope = {}
        if spec["role"] == "layer" and owner.routing is not None:
            enclosing = owner.capture.scopes[owner.capture.current.get()]
            parameters, relations = owner.routing.geometry.scope(enclosing, spec["expert_layer"])
            scope = {"parameters": parameters, "arguments": relations, "constants": enclosing["constants"]}
        with owner.capture.call(
            spec["role"], path, spec["implementation"],
            bindings={"weights": spec["weights"]}, device=True, **scope,
        ) as call:
            if cache is not None:
                cache.ports(call, "input")
            for name, tensor in inputs:
                rule = self.shape(spec, name, tensor, output=False)
                call.port(name, "input", owner.tensors.observe(tensor, rule), use="device" if tensor.device.type == "npu" else "host")
            result = original(*args, **kwargs)
            if cache is not None:
                cache.ports(call, "output")
            for name, tensor in tensor_ports(result, "result"):
                rule = self.shape(spec, name, tensor, output=True)
                call.port(name, "output", owner.tensors.observe(tensor, rule), use="device" if tensor.device.type == "npu" else "host")
            return result
