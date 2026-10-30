"""Observe native prefix services through reusable index operations, independent of tree size."""

from __future__ import annotations

from typing import Any

import torch
from torch.overrides import TorchFunctionMode

from markov_collection.adapters.runtimes.pytorch.concatenation import ConcatenationObservation
from markov_collection.adapters.runtimes.pytorch.indexing import TensorAssignmentCall, TensorViewCall
from markov_collection.instrumentation.calls import NativeCall


class CacheViewCall(TensorViewCall):
    def __init__(self, values: Any, original: Any, tensor: Any, index: Any) -> None:
        self.tensor = tensor
        self.row = index[0] if tensor.ndim == 2 else None
        bounds = index[1] if self.row is not None else index
        begin, end, step = bounds.indices(tensor.shape[-1])
        if step != 1:
            raise ValueError("native cache indices require contiguous token slices")
        self.begin, self.extent = begin, max(0, end - begin)
        symbols = values.shapes
        offset = symbols.symbol("index_begin")
        if self.row is not None:
            offset += symbols.symbol("request_slot") * symbols.symbol("request_map_stride")
        super().__init__(
            values.recording,
            values,
            original.__qualname__,
            original,
            {},
            interface="buffer_view",
            geometry=(symbols.symbol("index_tokens"),),
            start=offset,
        )

    def resources(self, arguments: dict[str, Any]) -> tuple[Any, Any]:
        self.input_port = self.values.value(self.tensor)
        return (self.tensor,), ()

    def completed(self, args: tuple, kwargs: dict, result: Any, metadata: dict) -> None:
        super().completed(args, kwargs, result, metadata)
        metadata.update(
            entrypoint="cache_map_read" if self.row is not None else "cache_index_slice",
            entrypoint_phase=None,
            parameters={"index_begin": self.begin, "index_tokens": self.extent},
            ports={"inputs": {"indices": self.input_port}, "outputs": self.values.port_values({"indices": result})},
        )
        if self.row is not None:
            metadata["parameters"]["request_slot"] = self.row
            metadata["bindings"] = {"request_map_stride": self.tensor.stride(0)}


class CacheCopyCall(NativeCall):
    def __init__(self, values: Any, original: Any, args: tuple, kwargs: dict) -> None:
        self.operation, self.arguments_tail, self.keywords = original, args[1:], kwargs
        super().__init__(values.recording, values, original.__qualname__, self.copy, {"indices": {0: "index_tokens"}})

    def copy(self, indices: Any) -> Any:
        return self.operation(indices, *self.arguments_tail, **self.keywords)

    def completed(self, args: tuple, kwargs: dict, result: Any, metadata: dict) -> None:
        metadata.update(
            entrypoint="cache_index_copy",
            entrypoint_phase=None,
            parameters={"index_tokens": args[0].numel()},
            ports={
                "inputs": self.values.port_values({"indices": args[0]}),
                "outputs": self.values.port_values({"indices": result}),
            },
        )


class CacheMapWriteCall(TensorAssignmentCall):
    def __init__(self, values: Any, original: Any, args: tuple) -> None:
        self.mapping, self.index, self.payload = args
        super().__init__(values.recording, values, original.__qualname__, original, {}, interface="tensor_assignment")

    def resources(self, arguments: dict[str, Any]) -> tuple[Any, Any]:
        self.input_ports = self.values.port_values({"request_map": self.mapping, "indices": self.payload})
        self.values.bind_shape(self.payload, (self.values.shapes.symbol("index_tokens"),))
        return (), (self.mapping,)

    def completed(self, args: tuple, kwargs: dict, result: Any, metadata: dict) -> None:
        slot, bounds = self.index
        begin, end, step = bounds.indices(self.mapping.shape[1])
        if step != 1 or end - begin != self.payload.numel():
            raise ValueError("native cache map write must replace its declared contiguous token interval")
        metadata.update(
            entrypoint="cache_map_write",
            entrypoint_phase=None,
            parameters={"request_slot": slot, "index_begin": begin, "index_tokens": self.payload.numel()},
            argument_relations=[
                {"value": self.input_ports["request_map"]},
                [
                    {"scalar": "request_slot"},
                    {"slice": [{"scalar": "index_begin"}, {"scalar": ["add", "index_begin", "index_tokens"]}, None]},
                ],
                {"value": self.input_ports["indices"]},
            ],
            ports={"inputs": self.input_ports, "outputs": self.values.port_values({"request_map": self.mapping})},
        )


class CacheIndexObservation(TorchFunctionMode):
    def __init__(self, values: Any) -> None:
        super().__init__()
        self.values = values
        self.scope = values.recording.current_scope.get()

    def __torch_function__(self, function: Any, types: Any, args: tuple = (), kwargs: dict | None = None) -> Any:
        kwargs = kwargs or {}
        if self.values.recording.current_scope.get() != self.scope:
            return function(*args, **kwargs)

        if function is torch.Tensor.__getitem__:
            with self.values.metadata():
                call = CacheViewCall(self.values, function, *args)
            return call(*args, **kwargs)
        if function is torch.Tensor.__setitem__:
            return CacheMapWriteCall(self.values, function, args)(*args, **kwargs)
        if function in (torch.Tensor.clone, torch.Tensor.to):
            return CacheCopyCall(self.values, function, args, kwargs)(args[0])
        return function(*args, **kwargs)


class PrefixServiceCall(NativeCall):
    def invoke(self, *args: Any, **kwargs: Any) -> Any:
        metadata = self.recording.regions[self.recording.current_scope.get()]["metadata"]
        metadata.update(
            entrypoint="cache_" + self.original.__name__.lstrip("_"), entrypoint_phase=None, dynamic_calls=True
        )

        # Only actual tensor work becomes reusable children; private tree traversal stays in CPU service.
        with CacheIndexObservation(self.values), ConcatenationObservation(self.values):
            return super().invoke(*args, **kwargs)


class PrefixObservation:
    def __init__(self, values: Any, empty_match: Any) -> None:
        self.values = values
        self.empty_match = empty_match

    @staticmethod
    def declare(regions: list[dict], values: list[dict]) -> None:
        for region in regions:
            metadata = region["metadata"]
            if metadata.get("entrypoint") != "cache_match_prefix":
                continue

            # MatchResult is a native tuple: the first field is the actual returned index tensor, including misses.
            metadata["ports"] = {"inputs": {}, "outputs": {"prefix_indices": metadata["result"][0]["value"]}}
            if values[metadata["result"][0]["value"]]["shape"] == [0]:
                metadata["entrypoint"] = "cache_empty"
                metadata.pop("dynamic_calls", None)

    def service(self, original: Any, cache: Any, *args: Any, **kwargs: Any) -> Any:

        # The scheduler's waiting-queue tree holds logical keys, not physical KV tensors.
        if cache.token_to_kv_pool_allocator is None or self.values.recording.current_scope.get() is None:
            return original(cache, *args, **kwargs)
        if cache.disable:
            if original.__name__ == "match_prefix":
                return self.empty_match(original, cache, *args, **kwargs)
            return original(cache, *args, **kwargs)

        return PrefixServiceCall(self.values.recording, self.values, original.__qualname__, original, {})(
            cache, *args, **kwargs
        )

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        for method in (
            "match_prefix",
            "insert",
            "_insert_helper",
            "_split_node",
            "inc_lock_ref",
            "dec_lock_ref",
            "evict",
        ):
            HookRegistry.register(
                f"sglang.srt.mem_cache.radix_cache.RadixCache.{method}", self.service, HookType.AROUND
            )
