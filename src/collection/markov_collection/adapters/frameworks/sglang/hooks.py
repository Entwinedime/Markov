"""Install native observation at SGLang's existing plugin boundary."""

from __future__ import annotations

import json
import os
import sys
from contextlib import nullcontext
from pathlib import Path
from typing import Any

from markov_collection.instrumentation.recording import Recording


class SGLangObservation:
    def __init__(self, description: dict[str, Any]) -> None:
        from torch.utils.weak import WeakIdKeyDictionary

        from markov_collection.adapters.frameworks.sglang.primitives import TokenRelayPrototypes
        from markov_collection.adapters.runtimes.ascend.concatenation import AscendConcatenation
        from markov_collection.adapters.runtimes.ascend.matrices import AscendMatrices
        from markov_collection.adapters.runtimes.ascend.observation import AscendOperatorObservation
        from markov_collection.adapters.runtimes.ascend.synchronization import SynchronizationObservation
        from markov_collection.adapters.runtimes.ascend.workspaces import AscendWorkspaces
        from markov_collection.adapters.runtimes.pytorch.observation import TorchMarkers

        self.directory = Path(description["output"])
        self.recording = Recording(TorchMarkers())
        self.synchronization = SynchronizationObservation(self.recording.markers)
        self.workspaces = AscendWorkspaces(self.synchronization, Path(description["native_library"]))
        self.workspace_states: dict[str, list[dict[str, int]]] = {}
        self.operators = AscendOperatorObservation(self.recording)
        self.concatenation = AscendConcatenation(self.operators)
        selection = description["matrix_selection"]
        self.matrices = AscendMatrices(
            self.operators, selection["binary"], selection["limits"], self.concatenation.version
        )
        self.allocation: Any = None
        self.materializations: Any = None
        self.token_relay = TokenRelayPrototypes(self.operators, selection["limits"]["B"])

        self.requests = set(description["source_requests"])
        self.baseline_requests = set(description["baseline_requests"])
        self.shard = None
        if "shard" in description:
            from markov_collection.adapters.frameworks.sglang.parallel import SingleDeviceShard

            shard = description["shard"]
            self.shard = SingleDeviceShard(shard["size"], shard["rank"], self.operators.values)

        self.model: Any = None
        self.batch_ids: Any = WeakIdKeyDictionary()

    def load_model(self, original: Any, runner: Any) -> Any:
        result = original(runner)
        self.model = runner.model
        self.implementation = {
            "model_path": runner.server_args.model_path,
            "model_config": runner.model_config.hf_config.to_dict(),
            "dtype": str(runner.model_config.dtype),
            "attention_backend": runner.server_args.attention_backend,
            "load_format": runner.server_args.load_format,
        }

        return result

    def record_batch(self, scheduler: Any, batch: Any, observation: str) -> int:
        phase = "control"
        members = []
        if batch is not None:
            phase = "prefill" if batch.forward_mode.is_extend() else "decode"
            members = [
                {
                    "id": request.rid,
                    "new_tokens": request.extend_input_len if phase == "prefill" else 1,
                    "context_tokens": len(request.prefix_indices)
                    if phase == "prefill"
                    else len(request.origin_input_ids) + len(request.output_ids) - 1,
                    "slot": int(request.req_pool_idx),
                }
                for request in batch.reqs
            ]

        identity = len(self.recording.batches)
        self.recording.batches.append(
            {
                "id": identity,
                "phase": phase,
                "rank": scheduler.ps.tp_rank,
                "members": members,
                "parameters": {
                    "M": sum(item["new_tokens"] for item in members),
                    "B": len(members),
                    "R": scheduler.req_to_token_pool.size,
                    "max_sequence_tokens": max(
                        (item["context_tokens"] + item["new_tokens"] for item in members), default=0
                    ),
                },
                "observation": observation,
            }
        )
        if batch is not None:
            self.batch_ids[batch] = identity
        return identity

    def schedule(self, original: Any, scheduler: Any) -> Any:
        pending = list(scheduler.waiting_queue)
        for batch in (scheduler.running_batch, scheduler.last_batch):
            if batch is not None:
                pending.extend(batch.reqs)

        # Finished members still participate in the native filter before the scheduler becomes idle.
        request_ids = {request.rid for request in pending}
        if request_ids and request_ids <= self.requests:
            observation = "source"
        elif request_ids and request_ids <= self.baseline_requests:
            observation = "baseline"
        else:
            return original(scheduler)

        identity = len(self.recording.batches)
        if observation == "source":
            self.operators.begin_batch()
            with self.operators.values.metadata():
                mapping = scheduler.req_to_token_pool.req_to_token
                shapes = self.operators.values.shapes

                # Pool capacity belongs to every value version, before preparation first reads or writes it.
                shapes.bind_shape(mapping, (shapes.symbol("R") + 1, mapping.shape[1]))
        context = (
            self.recording.scope("Schedule")
            if observation == "source"
            else self.recording.markers.scope(f"markov.baseline/{identity}")
        )
        with context as scope, self.operators if observation == "source" else nullcontext():
            if observation == "source":
                scope.metadata["ports"] = {
                    "inputs": self.operators.values.port_values(
                        {"free_pages": scheduler.token_to_kv_pool_allocator.free_pages}
                    ),
                    "outputs": {},
                }

            batch = original(scheduler)
            identity = self.record_batch(scheduler, batch, observation)
            if observation == "source":
                scope.metadata["batch"] = identity
            if batch is not None:
                if observation == "source":
                    scope.metadata["dynamic_calls"] = True
                    scope.metadata["parameters"] = {"scheduled_members": len(batch.reqs)}
                    scope.metadata["ports"]["outputs"] = self.operators.values.port_values(
                        {
                            "request_slots": batch.req_pool_indices,
                            "sequence_lengths": batch.seq_lens,
                            "kv_locations": batch.out_cache_loc,
                            "free_pages": scheduler.token_to_kv_pool_allocator.free_pages,
                        }
                    )
            elif observation == "source":
                scope.metadata["no_batch"] = True

            return batch

    def run_batch(self, original: Any, scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        from sglang.srt.layers.radix_attention import RadixAttention

        from markov_collection.adapters.frameworks.sglang.attention import AttentionCall
        from markov_collection.adapters.frameworks.sglang.batches import BatchTensors
        from markov_collection.adapters.frameworks.sglang.preparation import BatchPreparationCall
        from markov_collection.adapters.runtimes.pytorch.modules import ModuleObservation

        identity = self.batch_ids.get(batch)
        if identity is None:
            return original(scheduler, batch, *args, **kwargs)

        phase = self.recording.batches[identity]["phase"]
        if self.recording.batches[identity]["observation"] == "baseline":
            with self.recording.markers.scope(f"markov.baseline/{identity}"):
                return original(scheduler, batch, *args, **kwargs)

        # Decode gathers relayed tokens before the model wrapper binds its inputs.
        with self.recording.scope(phase.capitalize(), batch=identity) as scope, self.operators:
            with self.operators.values.metadata():

                # The serving model runs only nonempty batches; this is an input contract, not a source length hint.
                for name in ("M", "B"):
                    self.operators.values.bounded_parameter(name, 1, sys.maxsize)
                self.operators.values.bind(batch.req_pool_indices, {0: "B"})
                self.operators.values.bind_shape(
                    scheduler.future_map.output_tokens_buf, (self.operators.values.shapes.symbol("R") + 1,)
                )

            scope.metadata["ports"] = {
                "inputs": self.operators.values.port_values(
                    {
                        **BatchPreparationCall.model_inputs(batch),
                        "token_relay": scheduler.future_map.output_tokens_buf,
                    }
                ),
                "outputs": {},
            }

            # Bind actual model inputs after native staging, through SGLang's common model interface.
            # Decode contributes one token per member, so its token axis is B by definition.
            token_axis = "B" if phase == "decode" else "M"
            inputs = {
                "input_ids": {0: token_axis},
                "positions": {0: token_axis},
                "forward_batch.out_cache_loc": {0: token_axis},
                "forward_batch.seq_lens": {0: "B"},
                "forward_batch.req_pool_indices": {0: "B"},
                "forward_batch.extend_seq_lens": {0: "B"},
            }
            with ModuleObservation(
                self.recording, self.model, self.operators.values, inputs, {RadixAttention: AttentionCall}
            ):
                result = original(scheduler, batch, *args, **kwargs)

            scope.metadata["ports"]["outputs"] = self.operators.values.port_values(
                {
                    **BatchTensors.read(batch),
                    "sampled_tokens": result.next_token_ids,
                    "token_relay": scheduler.future_map.output_tokens_buf,
                }
            )
            return result

    def forward(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        if self.recording.current_scope.get() is None:
            return original(*args, **kwargs)

        with self.recording.scope("ModelSubmit"):
            return original(*args, **kwargs)

    def process_result(self, original: Any, scheduler: Any, batch: Any, result: Any) -> Any:
        identity = self.batch_ids.get(batch)
        if identity is None:
            return original(scheduler, batch, result)
        if self.recording.batches[identity]["observation"] == "baseline":
            with self.recording.markers.scope(f"markov.baseline/{identity}"):
                return original(scheduler, batch, result)

        with self.recording.scope("ResultProcessed", batch=identity) as scope, self.operators:
            scope.metadata["ports"] = {
                "inputs": self.operators.values.port_values(
                    {
                        "sampled_tokens": result.next_token_ids,
                        "free_pages": scheduler.token_to_kv_pool_allocator.free_pages,
                    }
                ),
                "outputs": {},
            }

            returned = original(scheduler, batch, result)
            scope.metadata["ports"]["outputs"] = self.operators.values.port_values(
                {"free_pages": scheduler.token_to_kv_pool_allocator.free_pages}
            )

            # Capture effects after native result handling has updated request lifetimes.
            with self.operators.values.metadata():
                scope.metadata["effects"] = [
                    {"id": request.rid, "generated_tokens": len(request.output_ids), "finished": request.finished()}
                    for request in batch.reqs
                ]

            return returned

    def publish(self, original: Any, sender: Any, output: Any, *args: Any, **kwargs: Any) -> Any:
        from sglang.srt.managers.io_struct import BatchTokenIDOutput

        if (
            self.recording.current_scope.get() is None
            or not isinstance(output, BatchTokenIDOutput)
            or sender.socket is None
        ):
            return original(sender, output, *args, **kwargs)

        with self.recording.scope(
            "RequestOutput", requests=list(output.rids), generated_tokens=list(output.completion_tokens)
        ):
            return original(sender, output, *args, **kwargs)

    def stash(self, original: Any, relay: Any, indices: Any, payload: Any) -> Any:
        from markov_collection.instrumentation.calls import NativeCall

        if self.recording.current_scope.get() is None:
            return original(relay, indices, payload)

        values = self.operators.values

        # ReqToTokenPool reserves slot zero; FutureMap uses the same padded capacity.
        with values.metadata():
            values.bind_shape(relay.output_tokens_buf, (values.shapes.symbol("R") + 1,))

        return NativeCall(
            self.recording,
            values,
            "sglang.FutureMap.stash",
            original,
            {"future_indices": {0: "B"}, "payload": {0: "B"}},
        )(relay, indices, payload)

    def release_pages(self, original: Any, pool: Any, free_index: Any) -> Any:
        from markov_collection.adapters.frameworks.sglang.state import PageReleaseCall

        if self.recording.current_scope.get() is None:
            return original(pool, free_index)

        with self.operators.values.metadata():
            call = PageReleaseCall(self.recording, self.operators.values, original, pool)
        return call(pool, free_index)

    def request_cache(self, original: Any, cache: Any, request: Any, *args: Any, **kwargs: Any) -> Any:
        from markov_collection.adapters.frameworks.sglang.state import RequestCacheCall

        if self.recording.current_scope.get() is None:
            return original(cache, request, *args, **kwargs)

        call = RequestCacheCall(
            self.operators.values, original, cache, request, original.__name__ == "cache_finished_req"
        )
        return call(cache, request, *args, **kwargs)

    @staticmethod
    def result_interfaces(regions: list[dict[str, Any]]) -> None:
        for region in regions:
            metadata = region["metadata"]

            # The target controller supplies members; the source supplies this caller's own CPU service.
            if region["role"] == "ResultProcessed":
                metadata["dynamic_calls"] = True
                metadata["parameters"] = {"result_members": len(metadata["effects"])}
                continue
            if region["role"] == "RequestOutput":
                metadata["entrypoint"] = "request_output"
                metadata["parameters"] = {"published_requests": len(metadata["requests"])}
                continue
            if region["role"] != "HostRead" or metadata["path"] != "tolist":
                continue

            # Only the read of this result's sampled tokens is a result-stage entrypoint.
            parent = region["parent"]
            while parent is not None:
                caller = regions[parent]
                if caller["role"] == "ResultProcessed":
                    if metadata["source"] == caller["metadata"]["ports"]["inputs"]["sampled_tokens"]:
                        metadata["entrypoint"] = "result_tokens"
                        metadata["ports"] = {"inputs": {"tensor": metadata["source"]}, "outputs": {}}
                    break
                parent = caller["parent"]

    def flush(self, original: Any, scheduler: Any, empty_cache: bool = True) -> Any:

        # Reset logical pools without discarding the allocator state established by uniform warmup.
        result = original(scheduler, empty_cache=False)
        if result:
            from markov_collection.adapters.frameworks.sglang.allocation import SGLangAllocation
            from markov_collection.adapters.frameworks.sglang.primitives import SGLangPrimitives
            from markov_collection.adapters.runtimes.ascend.materialization import AscendMaterializations

            if self.allocation is None:
                pool = scheduler.token_to_kv_pool_allocator
                self.allocation = SGLangAllocation(self.operators, pool.page_size, str(pool.device))
                self.materializations = AscendMaterializations(
                    self.operators,
                    pool.page_size,
                    scheduler.req_to_token_pool.req_to_token.dtype,
                    ("B", "max_sequence_tokens"),
                )
                weights, attention = SGLangPrimitives.model_resources(self.model)
                self.matrices.prepare(weights)
                self.workspace_policy = self.workspaces.reserve(self.matrices.description["cube_cores"], attention)
            self.concatenation.warmup()
            self.allocation.warmup()
            self.materializations.warmup()
            self.token_relay.warmup()

            boundary = self.recording.batches[-1]["observation"] if self.recording.batches else "warmup"
            self.workspace_states[boundary] = self.workspaces.snapshot()

            if any(batch["observation"] == "source" for batch in self.recording.batches):
                self.concatenation.capture()
                self.allocation.capture()
                self.matrices.capture()
                self.materializations.capture()
                self.token_relay.capture()
        if result and self.recording.batches:
            import torch
            import torch_npu

            self.result_interfaces(self.recording.regions)
            self.concatenation.declare()
            self.matrices.declare()
            if self.shard is not None:
                from markov_collection.adapters.frameworks.sglang.parallel import CollectiveCall

                CollectiveCall.bind_transports(self.recording.regions, self.recording.operations, self.recording.values)

            self.recording.write(
                self.directory / f"rank-{scheduler.ps.tp_rank}.json",
                {
                    "framework": "sglang",
                    "torch": torch.__version__,
                    "torch_npu": torch_npu.__version__,
                    "cann": self.concatenation.version,
                    "matrix_selection": self.matrices.description,
                    "observer": __file__,
                    "rank": scheduler.ps.tp_rank,
                    "logical_tp": self.shard.size if self.shard is not None else 1,
                    "source_transport": "physical_group_of_one",
                    "implementation": self.implementation,
                    "device": torch.npu.get_device_name(),
                    "device_index": torch.npu.current_device(),
                    "workspace_boundaries": self.workspace_states,
                    "workspace_policy": self.workspace_policy,
                },
            )
            self.synchronization.write(self.directory / "runtime")

        return result

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        from markov_collection.adapters.frameworks.sglang.preparation import SGLangPreparation
        from markov_collection.adapters.frameworks.sglang.primitives import SGLangPrimitives
        from markov_collection.adapters.runtimes.ascend.functions import AscendFunctionObservation

        functions = AscendFunctionObservation(self.operators.values)
        SGLangPrimitives(self.operators.values).install()
        SGLangPreparation(self.operators.values).install()

        # Framework-specific hook paths stay at this installation boundary.
        targets = {
            "sglang.srt.model_executor.model_runner.ModelRunner.load_model": self.load_model,
            "sglang.srt.model_executor.model_runner.ModelRunner.forward": self.forward,
            "sglang.srt.managers.scheduler.Scheduler.run_batch": self.run_batch,
            "sglang.srt.managers.scheduler.Scheduler.get_next_batch_to_run": self.schedule,
            "sglang.srt.managers.scheduler.Scheduler.process_batch_result": self.process_result,
            "sglang.srt.managers.scheduler.Scheduler.flush_cache": self.flush,
            "sglang.srt.managers.scheduler_components.output_sender.SenderWrapper.send_output": self.publish,
            "sglang.srt.managers.overlap_utils.FutureMap.stash": self.stash,
            "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator.free": self.release_pages,
            "sglang.srt.mem_cache.chunk_cache.ChunkCache.cache_finished_req": self.request_cache,
            "sglang.srt.mem_cache.chunk_cache.ChunkCache.cache_unfinished_req": self.request_cache,
            functions.target: functions.split_qkv,
        }
        for target, observer in targets.items():
            HookRegistry.register(target, observer, HookType.AROUND)

        if self.shard is not None:
            self.shard.install()


def install() -> None:
    description = os.environ.get("MARKOV_GRAPH_SESSION")
    if description is not None:
        SGLangObservation(json.loads(Path(description).read_text())).install()
