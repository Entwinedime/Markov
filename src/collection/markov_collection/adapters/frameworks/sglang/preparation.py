"""Bind native request arrays, allocations and input staging to batch geometry."""

from __future__ import annotations

from contextlib import contextmanager
from contextvars import ContextVar
from time import perf_counter_ns
from typing import Any

import torch
from torch.overrides import TorchFunctionMode

from markov_collection.adapters.frameworks.sglang.batches import (
    BatchTensors,
    BatchTransitionCall,
    BatchTransitionPrototypes,
)
from markov_collection.adapters.frameworks.sglang.state import PageAllocationCall, TokenAllocationCall
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

    @staticmethod
    def declare(source: dict[str, Any]) -> None:
        used = {
            member["value"]
            for region in source["regions"]
            for member in region["metadata"].get("input_groups", {}).get("prefixes", {}).get("members", [])
        }
        for region in source["regions"]:
            metadata = region["metadata"]
            if metadata.get("entrypoint") != "prefix_match":
                continue

            # Candidate lookups stay in caller CPU service unless preparation consumes their result.
            if metadata["ports"]["outputs"]["prefix_indices"] not in used:
                del metadata["entrypoint"]


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

        with (
            TensorGeometryObservation(self.values, ("B",), (batch.prefix_lens, batch.extend_lens)),
            PrefixLocations(self.values, [request.prefix_indices for request in batch.reqs]),
        ):
            return super().invoke(batch)


class PrefixLocationCall(NativeCall):
    def __init__(self, values: Any, function: Any, args: tuple, kwargs: dict) -> None:
        super().__init__(values.recording, values, "prefix_location", self.evaluate, {"prefix": {0: "extent"}})
        self.function, self.args, self.kwargs = function, args, kwargs

    def evaluate(self, prefix: Any) -> Any:

        # The nonempty branch reads the last existing index, including its storage dependency.
        with TensorViewObservation(
            self.values, [(prefix, (1,))], start=self.values.shapes.symbol("extent") - 1, index=slice(-1, None)
        ):
            return self.function(*self.args, **self.kwargs)

    def completed(self, args: tuple, kwargs: dict, result: Any, metadata: dict) -> None:
        prefix = args[0]
        self.declare(metadata, self.values.value(prefix), self.values.value(result), prefix.numel())

    @staticmethod
    def declare(metadata: dict, prefix: int, result: int, extent: int) -> None:
        metadata.update(
            entrypoint="prefix_location",
            entrypoint_phase=None,
            parameters={"extent": extent},
            guards=[{"condition": ["eq", "extent", 0], "expected": extent == 0}],
            ports={"inputs": {"prefix": prefix}, "outputs": {"location": result}},
        )


class PrefixLocations(TorchFunctionMode):
    @staticmethod
    def declare(source: dict) -> None:
        regions, values, operations = source["regions"], source["values"], source["operations"]
        for region in regions:
            metadata = region["metadata"]
            groups = metadata.get("input_groups", {})

            # Empty prefixes can be host tensors; each member's callable constrains its actual device.
            if "prefixes" in groups:
                groups["prefixes"].pop("device", None)
            declaration = metadata.get("group_repetitions", {}).get("segments", {})
            if declaration.get("mapping", {}).get("family") != "prefix_location":
                continue
            prefixes = regions[region["parent"]]["metadata"]["input_groups"]["prefixes"]["members"]
            for prefix, output in zip(prefixes, groups["segments"]["members"], strict=True):
                identity = operations[values[output["value"]]["producer"]]["region"]
                while regions[identity]["parent"] != region["parent"]:
                    identity = regions[identity]["parent"]
                producer = regions[identity]["metadata"]
                if producer.get("entrypoint") == "prefix_location":
                    continue
                if producer.get("interface") != "tensor_from_data" or prefix["parameters"]["extent"] != 0:
                    raise ValueError("prefix location requires its native empty factory or tail view")

                # The existing factory is the whole empty branch; retain its recorded host and device work.
                PrefixLocationCall.declare(producer, prefix["value"], output["value"], 0)
                producer["inputs"].append(prefix["value"])
                producer.setdefault("shape_contracts", []).append({"value": prefix["value"], "shape": ["extent"]})

    def __init__(self, values: Any, prefixes: list[Any]) -> None:
        super().__init__()
        self.values = values
        self.prefixes = {id(prefix): prefix for prefix in prefixes}
        self.scope = values.recording.current_scope.get()

    def __torch_function__(self, function: Any, types: Any, args: tuple = (), kwargs: dict | None = None) -> Any:
        kwargs = kwargs or {}
        if self.values.recording.current_scope.get() != self.scope or function is not torch.Tensor.__getitem__:
            return function(*args, **kwargs)
        prefix = self.prefixes.get(id(args[0]))
        if prefix is None or args[1] != slice(-1, None):
            return function(*args, **kwargs)
        return PrefixLocationCall(self.values, function, args, kwargs)(prefix)


class ForwardBatchCall(NativeCall):
    def invoke(self, cls: Any, batch: Any, model_runner: Any) -> Any:
        with TensorGeometryObservation(self.values, ("B",), (batch.prefix_lens, batch.extend_lens)):
            return super().invoke(cls, batch, model_runner)


class BatchPreparationCall(TensorGeometryCall):
    def __init__(
        self, preparation: SGLangPreparation, original: Any, batch: Any, decode: bool
    ) -> None:
        values = preparation.values
        self.preparation = preparation
        self.fields = preparation.fields if decode else BatchTensors.fields
        inputs = {f"self.{path}": {0: "B"} for path in self.fields.values()} if decode else {}
        super().__init__(values.recording, values, original.__qualname__, original, inputs, data_shape=("B",))
        self.batch = batch
        self.decode = decode

    def invoke(self, *args: Any, **kwargs: Any) -> Any:
        metadata = self.recording.regions[self.recording.current_scope.get()]["metadata"]
        token = self.preparation.current_preparation.set(metadata)
        try:
            return super().invoke(*args, **kwargs)
        finally:
            self.preparation.current_preparation.reset(token)

    @staticmethod
    def model_inputs(batch: Any, fields: dict[str, str] = BatchTensors.fields) -> dict[str, Any]:
        inputs = {
            **BatchTensors.read(batch, fields),
            "kv_locations": batch.out_cache_loc,
        }
        if batch.forward_mode.is_extend():
            inputs["prompt_tokens"] = batch.prefill_input_ids_cpu
        if batch.forward_mode.is_mixed():
            inputs["mixed_slots"] = batch.mix_running_indices

        return inputs

    def resources(self, arguments: dict[str, Any]) -> tuple[Any, Any]:
        batch = self.batch
        metadata = self.recording.regions[self.recording.current_scope.get()]["metadata"]
        metadata["requests"] = [request.rid for request in batch.reqs]
        metadata["parameters"] = {
            "B": len(batch.reqs),
            "M": len(batch.reqs) if self.decode else sum(request.extend_range.length for request in batch.reqs),
        }
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
        metadata["entrypoint_phase"] = "decode" if self.decode else "prefill"
        metadata["ports"] = {"inputs": self.input_ports, "outputs": self.values.port_values(self.output_ports)}

        # Batch columns must come from their producer; only persistent pools may be initialized here.
        metadata["required_inputs"] = sorted(set(self.input_ports) - {"request_map", "free_pages"})
        return None


class BatchMixCall(NativeCall):
    def __init__(self, values: Any, original: Any, batch: Any, running: Any) -> None:
        inputs = {
            **{f"self.{path}": {0: "prefill_members"} for path in BatchTensors.fields.values()},
            **{f"running_batch.{path}": {0: "decode_members"} for path in BatchTensors.fields.values()},
            "self.out_cache_loc": {0: "prefill_tokens"},
            "self.prefill_input_ids_cpu": {0: "prefill_tokens"},
            "running_batch.out_cache_loc": {0: "decode_members"},
        }
        super().__init__(values.recording, values, original.__qualname__, original, inputs)
        self.batch, self.running = batch, running
        self.parameters = {
            "prefill_members": len(batch.reqs),
            "decode_members": len(running.reqs),
            "prefill_tokens": batch.extend_num_tokens,
        }

    def resources(self, arguments: dict[str, Any]) -> tuple[Any, Any]:
        inputs = {
            **{f"prefill.{name}": tensor for name, tensor in BatchPreparationCall.model_inputs(self.batch).items()},
            **{f"decode.{name}": tensor for name, tensor in BatchPreparationCall.model_inputs(self.running).items()},
        }
        self.input_ports = self.values.port_values(inputs)
        return tuple(inputs.values()), ()

    def outputs(self, result: Any) -> Any:
        return tuple(self.output_tensors.values())

    def completed(self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]) -> None:
        metadata.update(
            entrypoint="batch_mix",
            entrypoint_phase="mixed",
            parameters=self.parameters,
            derived_parameters=[
                {"name": "B", "expression": ["add", "prefill_members", "decode_members"]},
                {"name": "M", "expression": ["add", "prefill_tokens", "decode_members"]},
            ],
        )
        self.output_tensors = BatchPreparationCall.model_inputs(self.batch)

        # Only native merge and concatenation produce the shared model columns.
        axes = {"kv_locations": "M", "prompt_tokens": "prefill_tokens", "mixed_slots": "decode_members"}
        for name, tensor in self.output_tensors.items():
            self.values.bind(tensor, {0: axes.get(name, "B")})
        metadata["ports"] = {"inputs": self.input_ports, "outputs": self.values.port_values(self.output_tensors)}
        metadata["required_inputs"] = sorted(self.input_ports)


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
    def __init__(
        self, operators: Any, fields: dict[str, str] = BatchTensors.fields, native_padding: bool = False
    ) -> None:
        self.values = operators.values
        self.fields = fields
        self.native_padding = native_padding
        self.transitions = BatchTransitionPrototypes(operators, fields)
        self.current_preparation: ContextVar[dict | None] = ContextVar("sglang_preparation", default=None)
        self.selection_views: ContextVar[dict[int, str]] = ContextVar("sglang_selection_views")

    @contextmanager
    def selection(self, batch: Any, view: str):
        if batch is None:
            yield
            return

        # Nested admission may select the running batch while the previous batch remains in scope.
        token = self.selection_views.set({**self.selection_views.get({}), id(batch): view})
        try:
            yield
        finally:
            self.selection_views.reset(token)

    def previous_selection(self, original: Any, scheduler: Any) -> Any:
        previous = scheduler.last_batch
        if previous is not None and not previous.forward_mode.is_extend():
            previous = None
        with self.selection(previous, "previous"):
            return original(scheduler)

    def running_selection(self, original: Any, scheduler: Any, batch: Any) -> Any:
        with self.selection(batch, "running"):
            return original(scheduler, batch)

    def admission_selection(self, original: Any, scheduler: Any) -> Any:
        with self.selection(scheduler.running_batch, "running"):
            return original(scheduler)

    def before_eviction(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        if (metadata := self.current_preparation.get()) is not None:

            # This is the control adapter's prepare_inputs seam, before the native capacity check.
            metadata["preparation_inputs_finished_ns"] = perf_counter_ns()
        return original(*args, **kwargs)

    def grouped_inputs(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        return ExtendAllocationCall(
            self.values.recording,
            self.values,
            original.__qualname__,
            original,
            {},
            repetition=GroupRepetition(
                "B", "map", {"group": "prefixes", "family": "prefix_location", "input": "prefix", "output": "location"}
            ),
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
        return BatchPreparationCall(self, original, batch, decode=False)(batch)

    def match_prefix(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        return PrefixMatchCall(self.values.recording, self.values, original.__qualname__, original, {})(*args, **kwargs)

    def prepare_decode(self, original: Any, batch: Any) -> Any:
        return BatchPreparationCall(self, original, batch, decode=True)(batch)

    def mix_batch(self, original: Any, batch: Any, running: Any) -> Any:
        return BatchMixCall(self.values, original, batch, running)(batch, running)

    def filter_batch(self, original: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if self.values.recording.current_scope.get() is None or not batch.reqs:
            return original(batch, *args, **kwargs)

        # Keep a valid input before native filtering can discard every member.
        self.transitions.remember(batch)
        selection = self.selection_views.get({}).get(id(batch))
        if selection is None and batch.forward_mode.is_prebuilt():

            # The prebuilt batch is selected after acceptance and before joining running Decode.
            selection = "previous"
        return BatchTransitionCall(
            self.values, original, batch, fields=self.fields, selection_view=selection
        )(batch, *args, **kwargs)

    def merge_batch(self, original: Any, batch: Any, other: Any) -> Any:
        if self.values.recording.current_scope.get() is None:
            return original(batch, other)

        return BatchTransitionCall(self.values, original, batch, other, fields=self.fields)(batch, other)

    def allocate_extend(self, original: Any, pool: Any, *args: Any, **kwargs: Any) -> Any:
        return PageAllocationCall(self.values, original, pool, decode=False)(pool, *args, **kwargs)

    def allocate_tokens(self, original: Any, pool: Any, *args: Any, **kwargs: Any) -> Any:
        result = TokenAllocationCall(self.values, original, pool)(pool, *args, **kwargs)
        if self.values.recording.current_scope.get() is not None:
            with self.values.metadata():

                # Publish the caller's geometry before copies propagate the allocator's local page-count symbols.
                self.values.bind(result, {0: "M"})

        return result

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
        )(*args, **kwargs)

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
            "sglang.srt.managers.scheduler.Scheduler.get_next_batch_to_run": self.previous_selection,
            "sglang.srt.managers.scheduler.Scheduler.process_prefill_chunk": self.previous_selection,
            "sglang.srt.managers.scheduler.Scheduler.update_running_batch": self.running_selection,
            "sglang.srt.managers.scheduler.Scheduler.get_new_batch_prefill": self.admission_selection,
            "sglang.srt.mem_cache.chunk_cache.ChunkCache.match_prefix": self.match_prefix,
            "sglang.srt.mem_cache.common.evict_from_tree_cache": self.before_eviction,
            "sglang.srt.mem_cache.common.alloc_for_extend": self.grouped_inputs,
            "sglang.srt.mem_cache.common.alloc_req_slots": self.request_slots,
            "sglang.srt.managers.schedule_batch.ScheduleBatch.prepare_for_extend": self.prepare_extend,
            "sglang.srt.managers.schedule_batch.ScheduleBatch.prepare_for_decode": self.prepare_decode,
            "sglang.srt.managers.schedule_batch.ScheduleBatch.mix_with_running": self.mix_batch,
            "sglang.srt.managers.schedule_batch.ScheduleBatch.filter_batch": self.filter_batch,
            "sglang.srt.managers.schedule_batch.ScheduleBatch.merge_batch": self.merge_batch,
            "sglang.srt.model_executor.forward_batch_info.ForwardBatch.init_new": self.forward_batch,
            "sglang.srt.sampling.sampling_batch_info.SamplingBatchInfo.from_schedule_batch": self.request_arrays,
            "sglang.srt.utils.common.flatten_arrays_to_pinned_cpu": self.materialize_tokens,
            "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator.alloc": self.allocate_tokens,
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
