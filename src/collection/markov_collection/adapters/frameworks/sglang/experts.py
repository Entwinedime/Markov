"""Observe native expert ownership, variable exchanges and grouped computation."""

from __future__ import annotations

import inspect
import sys
from functools import partial
from typing import Any

import torch

from markov_collection.adapters.runtimes.ascend.moe import ExpertPermutationCall
from markov_collection.adapters.runtimes.pytorch.collectives import CommunicationWork
from markov_collection.adapters.runtimes.pytorch.factories import TensorGeometryObservation
from markov_collection.adapters.runtimes.pytorch.indexing import TensorViewObservation
from markov_collection.instrumentation.calls import NativeCall


class ExpertRoutingCall(NativeCall):
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs, interface="expert_routing")
        self.layer = self.original.__self__

    def completed(self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]) -> None:
        hidden = args[0] if args else kwargs["hidden_states"]
        rows = self.values.shapes.tensor(hidden).shape[0]
        count = self.layer.topk_config.top_k
        self.values.bind_shape(result.topk_ids, (rows, count))
        self.values.bind_shape(result.topk_weights, (rows, count))
        metadata["attributes"] = {"top_k": count, "renormalize": self.layer.topk_config.renormalize}


class ExpertCall(NativeCall):
    def __init__(self, *args: Any, observation: ExpertObservation, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self.observation = observation
        self.layer = self.original.__self__

    def invoke(self, hidden_states: Any, topk_output: Any) -> Any:
        self.metadata = self.recording.regions[self.recording.current_scope.get()]["metadata"]
        layer = self.layer
        with self.values.metadata():
            self.hidden = hidden_states
            self.values.bounded_parameter("expert_received", 0, sys.maxsize)
            self.metadata.update(
                bindings={
                    "expert_layer": layer.layer_id,
                    "expert_rank": layer.moe_ep_rank,
                    "expert_begin": layer.moe_ep_rank * layer._num_local_routed,
                    "expert_count": layer._num_global_routed,
                    "local_expert_count": layer._num_local_routed,
                    "expert_top_k": layer.top_k,
                },
                attributes={
                    "experts": layer.num_experts,
                    "local_experts": layer.num_local_experts,
                    "top_k": layer.top_k,
                    "ep_size": layer.moe_ep_size,
                },
                ports={
                    "inputs": self.values.port_values(
                        {
                            "hidden": hidden_states,
                            **topk_output._asdict(),
                            **dict(layer.named_parameters(recurse=False)),
                        }
                    ),
                    "outputs": {},
                },
                parameters={},
            )

        # These are observations of data-dependent choices, never formulas inferred from token lengths.
        with self.values.auxiliary():
            self.metadata["source_routing"] = topk_output.topk_ids.cpu().tolist()

        if layer.moe_ep_size == 1:
            self.metadata["parameters"]["expert_received"] = sum(len(row) for row in self.metadata["source_routing"])

        previous = self.observation.current
        self.observation.current = self
        try:
            return super().invoke(hidden_states, topk_output)
        finally:
            self.observation.current = previous

    def completed(self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]) -> None:
        metadata.update(entrypoint="expert_layer", call_family="expert_layer")
        metadata["ports"]["outputs"] = self.values.port_values({"hidden": result})

        # Empty receivers select their own local branch, without duplicating whole model layouts.
        if self.layer.moe_ep_size > 1:
            metadata.setdefault("guards", []).append(
                {"condition": ["eq", "expert_received", 0], "expected": metadata["parameters"]["expert_received"] == 0}
            )


class ExpertComputeCall(NativeCall):
    def __init__(self, observation: ExpertObservation, original: Any) -> None:
        super().__init__(
            observation.recording, observation.values, original.__qualname__, original, {}, interface="expert_compute"
        )
        self.observation = observation

    def resources(self, arguments: dict[str, Any]) -> tuple[Any, Any]:
        return tuple(self.observation.current.layer.parameters(recurse=False)), ()

    def invoke(self, method: Any, layer: Any, dispatch_output: Any) -> Any:
        self.dispatch = dispatch_output
        return super().invoke(method, layer, dispatch_output)

    def completed(self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]) -> None:
        dispatch = self.dispatch
        self.values.bind_shape(result.hidden_states, self.values.shapes.tensor(dispatch.hidden_states).shape)
        metadata["attributes"] = self.observation.current.metadata["attributes"]
        arguments = self.values.encode((dispatch,))
        if self.observation.current.layer.moe_ep_size > 1:
            arguments[0][-1] = [
                {"scalar": f"expert_{index}"} for index in range(len(dispatch.num_recv_tokens_per_expert))
            ]
        metadata["argument_relations"] = arguments
        metadata["keywords"] = {"mapping": []}


class ExpertCollectiveCall(NativeCall):
    def __init__(self, observation: ExpertObservation, original: Any, gather: bool) -> None:
        super().__init__(
            observation.recording, observation.values, original.__qualname__, original, {}, interface="expert_exchange"
        )
        self.observation = observation
        self.gather = gather

    def outputs(self, result: Any) -> Any:
        return result if self.gather else result[1]

    def completed(self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]) -> None:
        frame = self.observation.current
        size = frame.layer.moe_ep_size
        source = self.values.shapes.tensor(args[0])
        communication = {"primitive": "all_gather" if self.gather else "all_to_all", "group": "ep", "size": size}
        metadata["argument_relations"] = [{"value": self.values.value(args[0])}]
        metadata["keywords"] = {"mapping": []}
        if self.gather:
            communication["dimension"] = 0
            self.values.bind_shape(result, (size * source.shape[0], *source.shape[1:]))
        else:
            sending, receiving = ("send", "receive") if frame.exchange == "dispatch" else ("receive", "send")
            communication.update(
                dimension=0,
                input_splits=[{"scalar": f"expert_{sending}_{index}"} for index in range(size)],
                output_splits=[{"scalar": f"expert_{receiving}_{index}"} for index in range(size)],
            )
            rows = self.values.shapes.symbol("expert_received")
            if frame.exchange == "combine":
                rows = self.values.shapes.tensor(frame.hidden).shape[0] * frame.layer.top_k
            self.values.bind_shape(result[1], (rows, *source.shape[1:]))
        metadata["communication"] = communication

        begin = self.recording.regions[self.recording.current_scope.get()]["operation_begin"]
        for operation in self.recording.operations[begin:]:
            if operation["name"].startswith("c10d."):
                operation["communication"] = True


class ExpertObservation:
    def __init__(self, serving: Any) -> None:
        self.recording = serving.recording
        self.values = serving.operators.values
        self.current: ExpertCall | None = None

    @staticmethod
    def declare(source: dict[str, Any]) -> None:
        if source["environment"]["implementation"]["deepep_mode"] != "normal":
            return

        # Fixed normal dispatch selects by tensor geometry and routes, independently of the serving phase.
        for region in source["regions"]:
            metadata = region["metadata"]
            if metadata.get("entrypoint") == "expert_layer":
                metadata["entrypoint_phase"] = None

    def module_calls(self, model: Any) -> dict[type, Any]:
        from sglang.srt.layers.moe.fused_moe_triton.layer import FusedMoE
        from sglang.srt.layers.moe.topk import TopK

        return {TopK: ExpertRoutingCall} | {
            type(module): partial(ExpertCall, observation=self)
            for module in model.modules()
            if isinstance(module, FusedMoE)
        }

    def layout(self, original: Any, buffer: Any, *args: Any, **kwargs: Any) -> Any:
        if self.current is None:
            return original(buffer, *args, **kwargs)

        from deep_ep.alltoall import alltoall_get_dispatch_layout

        code = inspect.unwrap(alltoall_get_dispatch_layout).__code__
        experts = self.current.layer._num_global_routed
        dimensions = {
            (code, "group_size"): buffer.group_size,
            (code, "num_experts"): experts,
            (code, "num_local_experts"): experts // buffer.group_size,
        }
        with self.values.metadata():
            rows = self.values.shapes.tensor(args[0]).shape[0]

        # DeepEP's token-membership buffer follows the local routing rows, not the acquisition batch length.
        with TensorGeometryObservation(
            self.values, factories={torch.zeros: (rows, buffer.group_size)}, scalars=dimensions
        ):
            result = original(buffer, *args, **kwargs)
        layout = buffer._alltoall_layout
        with self.values.metadata():
            TensorViewObservation.declare_slice(
                self.values,
                layout["num_global_tokens_per_local_expert"],
                "expert_begin",
                ["add", "expert_begin", "local_expert_count"],
            )
        parameters = self.current.metadata["parameters"]
        for prefix, field in (("send", "input_splits"), ("receive", "output_splits")):
            parameters.update({f"expert_{prefix}_{index}": int(count) for index, count in enumerate(layout[field])})
        parameters["expert_received"] = sum(layout["output_splits"])
        return result

    def dispatch(self, original: Any, buffer: Any, *args: Any, **kwargs: Any) -> Any:
        if self.current is None:
            return original(buffer, *args, **kwargs)

        self.current.exchange = "dispatch"
        result = original(buffer, *args, **kwargs)
        self.current.metadata["parameters"].update(
            {f"expert_{index}": int(count) for index, count in enumerate(result[3])}
        )
        return result

    def combine(self, original: Any, buffer: Any, *args: Any, **kwargs: Any) -> Any:
        if self.current is not None:
            self.current.exchange = "combine"
        return original(buffer, *args, **kwargs)

    def compute(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        if self.current is None:
            return original(*args, **kwargs)
        return ExpertComputeCall(self, original)(*args, **kwargs)

    def permutation(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        if self.current is None:
            return original(*args, **kwargs)

        return ExpertPermutationCall(self.values, original, "expert_received")(*args, **kwargs)

    def gather(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        if self.current is None:
            return original(*args, **kwargs)
        return ExpertCollectiveCall(self, original, True)(*args, **kwargs)

    def exchange(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        if self.current is None:
            return original(*args, **kwargs)

        source, target, work = ExpertCollectiveCall(self, original, False)(*args, **kwargs)
        return source, target, CommunicationWork(self.values, work, (source, target))

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        targets = {
            "deep_ep.Buffer.get_dispatch_layout": self.layout,
            "deep_ep.Buffer.dispatch": self.dispatch,
            "deep_ep.Buffer.combine": self.combine,
            "deep_ep.alltoall._gather_along_first_dim": self.gather,
            "deep_ep.alltoall.async_all_to_all": self.exchange,
            "sglang.srt.layers.quantization.unquant.UnquantizedFusedMoEMethod.forward_npu": self.compute,
            "torch.repeat_interleave": self.permutation,
            "torch_npu.npu_moe_token_permute": self.permutation,
            "torch_npu.npu_moe_token_unpermute": self.permutation,
        }
        for target, observer in targets.items():
            HookRegistry.register(target, observer, HookType.AROUND)
