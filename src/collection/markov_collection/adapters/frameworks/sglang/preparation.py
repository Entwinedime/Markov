"""Bind native request arrays, allocations and input staging to batch geometry."""

from __future__ import annotations

from typing import Any

import torch

from markov_collection.adapters.frameworks.sglang.batches import BatchTensors, BatchTransitionCall
from markov_collection.adapters.frameworks.sglang.state import PageAllocationCall
from markov_collection.adapters.runtimes.pytorch.concatenation import CollectionCall, GroupRepetition
from markov_collection.adapters.runtimes.pytorch.factories import (
    TensorCreationCall,
    TensorGeometryCall,
    TensorGeometryObservation,
)
from markov_collection.adapters.runtimes.pytorch.indexing import ScalarReductionObservation, TensorViewObservation
from markov_collection.instrumentation.calls import NativeCall


class PrefixMatchCall(NativeCall):
    def outputs(self, result: Any) -> Any:
        return result.device_indices

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:

        # Cold logical KV still obtains its empty prefix through the native cache interface.
        metadata["entrypoint"] = "prefix_match"
        metadata["ports"] = {
            "inputs": {},
            "outputs": self.values.port_values({"prefix_indices": result.device_indices}),
        }
        return None


class ExtendAllocationCall(CollectionCall):
    def invoke(self, batch: Any) -> Any:

        # Prefix members cross both preparation calls as one collection, not fixed source slots.
        with self.values.metadata():
            metadata = self.recording.regions[self.recording.current_scope.get()]["metadata"]
            metadata["input_groups"] = {
                "prefixes": self.values.input_group(
                    [request.prefix_indices for request in batch.reqs], 0, "B", "prefix_tokens"
                )
            }
            metadata["parent_groups"] = {"prefixes": "prefixes"}

        with TensorGeometryObservation(self.values, ("B",), (batch.prefix_lens, batch.extend_lens)):
            return super().invoke(batch)


class ForwardBatchCall(NativeCall):
    def invoke(self, cls: Any, batch: Any, model_runner: Any) -> Any:
        with TensorGeometryObservation(self.values, ("B",), (batch.prefix_lens, batch.extend_lens)):
            return super().invoke(cls, batch, model_runner)


class BatchPreparationCall(TensorGeometryCall):
    def __init__(
        self, values: Any, original: Any, batch: Any, decode: bool, fields: dict[str, str] = BatchTensors.fields
    ) -> None:
        self.fields = fields if decode else BatchTensors.fields
        inputs = {f"self.{path}": {0: "B"} for path in self.fields.values()} if decode else {}
        super().__init__(values.recording, values, original.__qualname__, original, inputs, data_shape=("B",))
        self.batch = batch
        self.decode = decode

    @staticmethod
    def model_inputs(batch: Any, fields: dict[str, str] = BatchTensors.fields) -> dict[str, Any]:
        inputs = {
            **BatchTensors.read(batch, fields),
            "kv_locations": batch.out_cache_loc,
        }
        if batch.forward_mode.is_extend():
            inputs["prompt_tokens"] = batch.prefill_input_ids_cpu

        return inputs

    def resources(self, arguments: dict[str, Any]) -> tuple[Any, Any]:
        batch = self.batch
        inputs = {
            "request_map": batch.req_to_token_pool.req_to_token,
            "free_pages": batch.token_to_kv_pool_allocator.free_pages,
        }
        prefixes = []
        if self.decode:
            inputs.update(BatchTensors.read(batch, self.fields))
            inputs["prefix_lengths"] = inputs.pop("sequence_lengths")
            inputs["prefix_lengths_cpu"] = inputs.pop("sequence_lengths_cpu")
        else:
            prefixes = [request.prefix_indices for request in batch.reqs]
            metadata = self.recording.regions[self.recording.current_scope.get()]["metadata"]
            metadata["input_groups"] = {"prefixes": self.values.input_group(prefixes, 0, "B", "prefix_tokens")}

        self.input_ports = self.values.port_values(inputs)

        # The mapping is opaque state; native allocation, indexing and length updates stay visible.
        return (batch.token_to_kv_pool_allocator.free_pages, *prefixes), (batch.req_to_token_pool.req_to_token,)

    def outputs(self, result: Any) -> Any:
        return tuple(self.output_ports.values())

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        batch = self.batch
        self.output_ports = {
            **self.model_inputs(batch, self.fields),
            "request_map": batch.req_to_token_pool.req_to_token,
            "free_pages": batch.token_to_kv_pool_allocator.free_pages,
        }
        metadata["entrypoint"] = "batch_preparation"
        metadata["ports"] = {"inputs": self.input_ports, "outputs": self.values.port_values(self.output_ports)}
        return None


class InputStagingCall(NativeCall):
    def __init__(self, *args: Any, native_padding: bool, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self.native_padding = native_padding

    def invoke(self, runner: Any, forward_batch: Any, *args: Any, **kwargs: Any) -> Any:
        with self.values.metadata():
            symbols = self.values.shapes
            members = symbols.symbol("model_members" if self.native_padding else "B")
            tokens = symbols.symbol("model_tokens" if self.native_padding else "M")
            buffers = []

            # The native registry owns slot axes and geometry, including non-leading token axes.
            for slot in runner._eager_registry._slots.values():
                if not slot.enabled or slot.buffer is None or slot.axis == "none" or slot.source_fn is not None:
                    continue

                geometry = tuple(slot.shape_fn(members, tokens))
                buffers.append((slot.buffer, geometry))
                source = getattr(forward_batch, slot.name)
                if isinstance(source, torch.Tensor):
                    self.values.bind_shape(source, geometry)

        with TensorViewObservation(self.values, buffers):
            return super().invoke(runner, forward_batch, *args, **kwargs)


class SGLangPreparation:
    def __init__(self, values: Any, fields: dict[str, str] = BatchTensors.fields, native_padding: bool = False) -> None:
        self.values = values
        self.fields = fields
        self.native_padding = native_padding

    def grouped_inputs(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        return ExtendAllocationCall(
            self.values.recording,
            self.values,
            original.__qualname__,
            original,
            {},
            repetition=GroupRepetition("B", "produce"),
        )(*args, **kwargs)

    def request_arrays(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        return TensorGeometryCall(
            self.values.recording, self.values, original.__qualname__, original, {}, data_shape=("B",)
        )(*args, **kwargs)

    def request_slots(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        result = original(*args, **kwargs)
        if self.values.tensor_geometry is not None:
            self.values.tensor_geometry.bind(result)
        return result

    def forward_batch(self, original: Any, cls: Any, batch: Any, model_runner: Any) -> Any:
        axis = "B" if batch.forward_mode.is_decode() else "M"

        return ForwardBatchCall(
            self.values.recording,
            self.values,
            original.__qualname__,
            original,
            {
                "batch.input_ids": {0: axis},
                "batch.out_cache_loc": {0: axis},
                **{f"batch.{name}": {0: "B"} for name in ("seq_lens", "seq_lens_cpu", "req_pool_indices")},
            },
            role="Prepare",
        )(cls, batch, model_runner)

    def prepare_extend(self, original: Any, batch: Any) -> Any:
        return BatchPreparationCall(self.values, original, batch, decode=False)(batch)

    def match_prefix(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        return PrefixMatchCall(self.values.recording, self.values, original.__qualname__, original, {})(*args, **kwargs)

    def prepare_decode(self, original: Any, batch: Any) -> Any:
        return BatchPreparationCall(self.values, original, batch, decode=True, fields=self.fields)(batch)

    def filter_batch(self, original: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if self.values.recording.current_scope.get() is None or not batch.reqs:
            return original(batch, *args, **kwargs)

        return BatchTransitionCall(self.values, original, batch, fields=self.fields)(batch, *args, **kwargs)

    def merge_batch(self, original: Any, batch: Any, other: Any) -> Any:
        if self.values.recording.current_scope.get() is None:
            return original(batch, other)

        return BatchTransitionCall(self.values, original, batch, other, fields=self.fields)(batch, other)

    def allocate_extend(self, original: Any, pool: Any, *args: Any, **kwargs: Any) -> Any:
        return PageAllocationCall(self.values, original, pool, decode=False)(pool, *args, **kwargs)

    def allocate_decode(self, original: Any, pool: Any, *args: Any, **kwargs: Any) -> Any:
        return PageAllocationCall(self.values, original, pool, decode=True)(pool, *args, **kwargs)

    def materialize_tokens(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        return TensorCreationCall(
            self.values.recording,
            self.values,
            original.__qualname__,
            original,
            {},
            interface="materialize_tokens",
            geometry=("M",),
            data_argument="parts",
        )(*args, **kwargs)

    def stage_inputs(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        return InputStagingCall(
            self.values.recording, self.values, original.__qualname__, original, {}, native_padding=self.native_padding
        )(
            *args, **kwargs
        )

    def attention_metadata(self, original: Any, backend: Any, forward_batch: Any) -> Any:
        if self.values.recording.current_scope.get() is None:
            return original(backend, forward_batch)

        with self.values.metadata():
            context_capacity = backend.req_to_token_pool.req_to_token.shape[1]

        with ScalarReductionObservation(
            self.values, forward_batch.seq_lens, "max_sequence_tokens", 1, context_capacity
        ):
            return original(backend, forward_batch)

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        # Hooks attach geometry at native materialization boundaries before dispatcher observation.
        targets = {
            "sglang.srt.mem_cache.chunk_cache.ChunkCache.match_prefix": self.match_prefix,
            "sglang.srt.mem_cache.common.alloc_for_extend": self.grouped_inputs,
            "sglang.srt.mem_cache.common.alloc_req_slots": self.request_slots,
            "sglang.srt.managers.schedule_batch.ScheduleBatch.prepare_for_extend": self.prepare_extend,
            "sglang.srt.managers.schedule_batch.ScheduleBatch.prepare_for_decode": self.prepare_decode,
            "sglang.srt.managers.schedule_batch.ScheduleBatch.filter_batch": self.filter_batch,
            "sglang.srt.managers.schedule_batch.ScheduleBatch.merge_batch": self.merge_batch,
            "sglang.srt.model_executor.forward_batch_info.ForwardBatch.init_new": self.forward_batch,
            "sglang.srt.sampling.sampling_batch_info.SamplingBatchInfo.from_schedule_batch": self.request_arrays,
            "sglang.srt.utils.common.flatten_arrays_to_pinned_cpu": self.materialize_tokens,
            "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator.alloc_extend": (
                self.allocate_extend
            ),
            "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator.alloc_decode": (
                self.allocate_decode
            ),
            "sglang.srt.model_executor.runner.eager_runner.EagerRunner.load_batch": self.stage_inputs,
            "sglang.srt.hardware_backend.npu.attention.ascend_backend.AscendAttnBackend.init_forward_metadata": (
                self.attention_metadata
            ),
        }
        for target, observer in targets.items():
            HookRegistry.register(target, observer, HookType.AROUND)
