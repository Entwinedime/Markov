"""Declare data and state ports for SGLang's direct batch preparation work."""

from __future__ import annotations

import inspect
from typing import Any

from markov_collection.adapters.runtimes.pytorch.kernels import KernelObservation
from markov_collection.instrumentation.calls import NativeCall


class IndexedWritePrototypes:
    def __init__(self, operators: Any, max_members: int) -> None:
        import torch

        from markov_collection.adapters.runtimes.ascend.indexed_writes import AscendIndexedWrites

        self.operators = operators
        self.recording = operators.recording
        self.warmed = False
        self.captured = False

        # Ordinary serving may never form B>1, so its write variants cannot depend on scheduler timing.
        self.points = [((3,), members, torch.int64) for members in (1, 2)]
        if max_members > AscendIndexedWrites.index_limit:
            self.points.append(
                ((AscendIndexedWrites.index_limit + 2,), AscendIndexedWrites.index_limit + 1, torch.int64)
            )
        if max_members >= AscendIndexedWrites.storage_limit:
            self.points.append(((AscendIndexedWrites.storage_limit + 1,), 1, torch.int64))

    def request_map(self, mapping: Any) -> None:
        self.points.extend(((3, mapping.shape[1]), members, mapping.dtype) for members in (1, 2))

    def tensors(self, shape: tuple[int, ...], members: int, dtype: Any) -> tuple[Any, Any, Any]:
        import torch

        indices = tuple(torch.arange(members, dtype=torch.int64, device="npu") for _ in shape)
        return (
            torch.zeros(shape, dtype=dtype, device="npu"),
            indices[0] if len(indices) == 1 else indices,
            torch.ones(members, dtype=dtype, device="npu"),
        )

    def warmup(self) -> None:
        import torch

        if self.warmed:
            return
        for point in self.points:
            target, indices, payload = self.tensors(*point)
            target[indices] = payload
        torch.npu.synchronize()
        self.warmed = True

    def capture(self) -> None:
        import sys

        import torch

        from markov_collection.adapters.runtimes.pytorch.observation import TorchMarkers

        if self.captured:
            return
        for shape, members, dtype in self.points:
            for observed in (False, True):
                with self.operators.values.auxiliary():
                    target, indices, payload = self.tensors(shape, members, dtype)
                identity = len(self.recording.batches)
                self.recording.batches.append(
                    {
                        "id": identity,
                        "kind": "primitive",
                        "phase": "primitive",
                        "members": [],
                        "parameters": {"slot_capacity": shape[0] - 1, "B": members},
                        "observation": "source" if observed else "baseline",
                    }
                )
                if observed:
                    self.operators.begin_batch()
                    with self.recording.scope("RuntimePrototype", batch=identity), self.operators:
                        with self.operators.values.metadata():
                            values = self.operators.values
                            capacity_axis = values.bounded_parameter("slot_capacity", 1, sys.maxsize)
                            member_axis = values.bounded_parameter("B", 1, sys.maxsize)
                            values.shapes.bind_contiguous(target, (capacity_axis + 1, *shape[1:]))
                            for tensor in values.tensor_leaves(indices):
                                values.shapes.bind_contiguous(tensor, (member_axis,))
                            values.shapes.bind_contiguous(payload, (member_axis,))
                        target[indices] = payload
                else:
                    with TorchMarkers.scope(f"markov.baseline/{identity}"):
                        target[indices] = payload
                torch.npu.synchronize()
        self.captured = True


class AllocateIndicesCall(NativeCall):
    def __init__(self, values: Any, kernel: Any, original: Any) -> None:
        super().__init__(
            values.recording,
            values,
            kernel.fn.__qualname__,
            original,
            {},
            interface="allocate_kv_indices",
            mutations=(4,),
        )
        self.signature = inspect.signature(kernel.fn)
        self.inputs = {
            "pre_lens_ptr": {0: "B"},
            "seq_lens_ptr": {0: "B"},
            "last_loc_ptr": {0: "B"},
            "out_indices": {0: "M"},
        }

    def resources(self, arguments: dict[str, Any]) -> tuple[Any, Any]:
        return (arguments["free_page_ptr"],), ()

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        metadata["entrypoint"] = "kv_indices"
        metadata["attributes"] = {"page_size": args[6]}
        self.values.bind_shape(args[4], (self.values.shapes.symbol("M"),))

        # Public work uses tensor ports; native tiling arguments remain raw source evidence.
        metadata["argument_relations"] = metadata["arguments"][:5]
        metadata["ports"] = {
            "inputs": {},
            "outputs": self.values.port_values({"kv_locations": args[4]}),
        }

        return None


class WriteRequestTokensCall(NativeCall):
    def resources(self, arguments: dict[str, Any]) -> tuple[Any, Any]:
        mapping = arguments["req_to_token_pool"].req_to_token
        self.mapping_input = self.values.value(mapping)

        self.input_ports = {"request_map": self.mapping_input}
        self.input_ports.update(
            self.values.port_values(
                {
                    "kv_locations": arguments["out_cache_loc"],
                    "request_slots": arguments["req_pool_indices_tensor"],
                    "request_slots_cpu": arguments["req_pool_indices_cpu"],
                    "sequence_lengths": arguments["seq_lens_tensor"],
                    "sequence_lengths_cpu": arguments["seq_lens_cpu"],
                    "prefix_lengths": arguments["prefix_lens_tensor"],
                    "prefix_lengths_cpu": arguments["prefix_lens_cpu"],
                    "new_lengths": arguments["extend_lens_tensor"],
                    "new_lengths_cpu": arguments["extend_lens_cpu"],
                }
            )
        )

        return arguments["prefix_tensors"], (mapping,)

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        metadata["entrypoint"] = "request_mapping"
        prefixes = args[9]
        total = sum(tensor.numel() for tensor in prefixes)
        metadata["parameters"] = {"B": len(prefixes), "prefix_tokens": total}
        metadata["input_groups"] = {"prefixes": self.values.input_group(prefixes, 0, "B", "prefix_tokens")}
        metadata["parent_groups"] = {"prefixes": "prefixes"}
        metadata["guards"] = [{"condition": ["eq", "prefix_tokens", 0], "expected": total == 0}]

        # A collection port keeps prefix count dynamic without storing one argument slot per request.
        metadata["argument_relations"] = metadata["arguments"][:9] + [
            {"input_group": "prefixes"},
            {"value": self.mapping_input},
        ]
        metadata["ports"] = {
            "inputs": self.input_ports,
            "outputs": self.values.port_values({"request_map": args[10].req_to_token}),
        }

        return None


class PositionCall(NativeCall):
    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        symbols = self.values.shapes
        self.values.bind_shape(result[0], (symbols.symbol("M"),))
        self.values.bind_shape(result[1], (symbols.symbol("B"),))

        metadata["entrypoint"] = "positions"
        metadata["attributes"] = {"has_prefix": args[0].shape[0] == args[1].shape[0]}
        metadata["argument_relations"] = metadata["arguments"][:2]
        metadata["ports"] = {
            "inputs": {},
            "outputs": self.values.port_values({"positions": result[0], "request_offsets": result[1]}),
        }

        return None


class VocabularyMaskCall(NativeCall):
    def __init__(self, values: Any, original: Any) -> None:
        super().__init__(values.recording, values, original.__qualname__, original, {}, interface="vocabulary_mask")
        self.mask_signature = inspect.signature(original)

    def completed(self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]) -> None:
        arguments = self.mask_signature.bind(*args, **kwargs).arguments
        tensor, *fields = arguments
        source = arguments[tensor]
        metadata["bindings"] = {"vocab_" + field: arguments[field] for field in fields}

        # API argument names identify shard offsets; observed integer equality never identifies a binding.
        metadata["argument_relations"] = [{"value": self.values.value(source)}] + [
            {"scalar": "vocab_" + field} for field in fields
        ]
        metadata["keywords"] = {"mapping": []}
        shape = self.values.shapes.tensor(source).shape
        for output in result:
            self.values.bind_shape(output, shape)


class SGLangPrimitives:
    def __init__(self, values: Any) -> None:
        self.values = values

    @staticmethod
    def attention_geometries(model: Any) -> list[tuple[int, int, int]]:
        from sglang.srt.layers.radix_attention import RadixAttention

        return sorted(
            {
                (module.tp_q_head_num, module.qk_head_dim, module.v_head_dim)
                for module in model.modules()
                if isinstance(module, RadixAttention)
            }
        )

    def positions(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        if self.values.recording.current_scope.get() is None:
            return original(*args, **kwargs)

        with self.values.metadata():
            inputs = {"extend_seq_lens": {0: "B"}}
            if args[0].shape[0] == args[1].shape[0]:
                inputs["extend_prefix_lens"] = {0: "B"}
            else:
                self.values.shapes.tensor(args[0])

        return PositionCall(
            self.values.recording,
            self.values,
            "sglang.compute_position_triton",
            original,
            inputs,
            interface="compute_positions",
        )(*args, **kwargs)

    def write_request_tokens(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        return WriteRequestTokensCall(
            self.values.recording,
            self.values,
            "sglang.write_cache_indices",
            original,
            {
                name: {0: "M" if name == "out_cache_loc" else "B"}
                for name in (
                    "out_cache_loc",
                    "req_pool_indices_tensor",
                    "req_pool_indices_cpu",
                    "prefix_lens_tensor",
                    "prefix_lens_cpu",
                    "seq_lens_tensor",
                    "seq_lens_cpu",
                    "extend_lens_tensor",
                    "extend_lens_cpu",
                )
            },
            interface="write_request_tokens",
        )(*args, **kwargs)

    def release_group(self, original: Any, pool: Any) -> Any:
        from markov_collection.adapters.frameworks.sglang.state import PageReleaseGroupCall

        if self.values.recording.current_scope.get() is None or not pool.free_group:
            return original(pool)
        return PageReleaseGroupCall(self.values.recording, self.values, original, pool)(pool)

    def vocabulary_mask(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        return VocabularyMaskCall(self.values, original)(*args, **kwargs)

    def install(self) -> None:
        from sgl_kernel_npu.mem_cache import allocator
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        allocator.alloc_extend_kernel = KernelObservation(
            self.values, allocator.alloc_extend_kernel, AllocateIndicesCall
        )

        targets = {
            "sglang.srt.layers.vocab_parallel_embedding.get_masked_input_and_mask": self.vocabulary_mask,
            "sglang.srt.mem_cache.common.write_cache_indices": self.write_request_tokens,
            "sglang.srt.model_executor.triton_ops.position.compute_position_triton": self.positions,
            "sglang.srt.mem_cache.allocator.base.BaseTokenToKVPoolAllocator.free_group_end": self.release_group,
        }
        for target, observer in targets.items():
            HookRegistry.register(target, observer, HookType.AROUND)
