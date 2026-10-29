"""Install native observation at SGLang's existing plugin boundary."""

from __future__ import annotations

import json
import os
import sys
from contextlib import nullcontext
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import write_json
from markov_collection.instrumentation.recording import Recording


class SGLangObservation:
    def __init__(self, description: dict[str, Any]) -> None:
        from torch.utils.weak import WeakIdKeyDictionary

        from markov_collection.adapters.frameworks.sglang.experts import ExpertObservation
        from markov_collection.adapters.frameworks.sglang.primitives import IndexedWritePrototypes
        from markov_collection.adapters.runtimes.ascend.concatenation import AscendConcatenation
        from markov_collection.adapters.runtimes.ascend.matrices import AscendMatrices
        from markov_collection.adapters.runtimes.ascend.observation import AscendOperatorObservation
        from markov_collection.adapters.runtimes.ascend.synchronization import SynchronizationObservation
        from markov_collection.adapters.runtimes.ascend.workspaces import AscendWorkspaces
        from markov_collection.adapters.runtimes.pytorch.observation import TorchMarkers

        self.directory = Path(description["output"])
        self.rank_offset = description["rank_offset"]
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
        self.indexed_writes = IndexedWritePrototypes(self.operators, selection["limits"]["B"])
        self.experts = ExpertObservation(self)
        self.ep_size = description["ep_size"]

        self.requests = set(description["source_requests"])
        self.baseline_requests = set(description["baseline_requests"])
        self.shard = None
        if "shard" in description:
            from markov_collection.adapters.frameworks.sglang.parallel import SingleDeviceShard

            shard = description["shard"]
            self.shard = SingleDeviceShard(shard["size"], shard["rank"], self.operators.values)

        self.model: Any = None
        self.model_stream: Any = None
        self.selected = True
        self.derived_role = False
        self.observe_all_workers = description.get("observe_all_workers", False)
        self.batch_ids: Any = WeakIdKeyDictionary()
        self.batch_lane = 0
        self.process_groups = None
        if (
            description["pp_size"] > 1
            or description["ep_size"] > 1
            or "disaggregation_role" in description
            or self.observe_all_workers
        ):
            from markov_collection.adapters.frameworks.sglang.parallel import ProcessGroupObservation

            self.process_groups = ProcessGroupObservation(self)

        self.pipeline = None
        if description["pp_size"] > 1:
            from markov_collection.adapters.frameworks.sglang.pipeline import PipelineObservation

            self.pipeline = PipelineObservation(self)

        self.disaggregation = None
        if "disaggregation_role" in description:
            from markov_collection.adapters.frameworks.sglang.disaggregation import DisaggregationObservation

            self.disaggregation = DisaggregationObservation(self, description["disaggregation_role"])

        self.routing = None
        if description.get("serving_input") is not None:
            from markov_collection.adapters.frameworks.sglang.native_routes import NativeExpertRoutes

            self.routing = NativeExpertRoutes(self.operators.values, description)

    def load_model(self, original: Any, runner: Any) -> Any:
        from sglang.srt.layers.radix_attention import RadixAttention
        from sglang.srt.runtime_context import get_parallel

        result = original(runner)
        self.model = runner.model

        if self.routing is not None:
            self.routing.configure(self.model, runner.model_config.hf_config.num_hidden_layers)

        tp_size = self.shard.size if self.shard is not None else runner.tp_size
        tp_rank = self.shard.rank if self.shard is not None else runner.tp_rank

        # Wrapper properties vary by model; the loaded attention modules identify the actual local layer interval.
        layers = [module.layer_id for module in self.model.modules() if isinstance(module, RadixAttention)]
        self.parallel = {
            "rank": self.rank_offset + runner.pp_rank * tp_size + tp_rank,
            "tp_rank": tp_rank,
            "pp_rank": runner.pp_rank,
            "logical_tp": tp_size,
            "logical_pp": runner.pp_size,
            "logical_ep": get_parallel().moe_ep_size,
            "ep_rank": get_parallel().moe_ep_rank,
            "layer_range": [min(layers), max(layers) + 1],
        }
        if self.process_groups is not None:
            from markov_collection.adapters.frameworks.sglang.role_plan import WorkerRolePlan

            plan = WorkerRolePlan.from_group(self.model, self.parallel)
            self.derived_role = self.parallel["rank"] not in plan["representatives"]
            self.selected = self.observe_all_workers or not self.derived_role
            plan["observed_roles"] = (
                list(range(self.rank_offset, self.rank_offset + plan["participants"]))
                if self.observe_all_workers
                else plan["representatives"]
            )
            self.synchronization.set_enabled(self.selected)
            if runner.pp_rank == 0 and tp_rank == 0:
                write_json(self.directory / "role-plan.json", plan, indent=2)
        elif self.shard is not None and tp_rank == 0:
            from markov_collection.adapters.frameworks.sglang.role_plan import WorkerRolePlan

            self.parallel["worker_roles"] = WorkerRolePlan.from_model(self.model, tp_size)
        self.implementation = {
            "model_path": runner.server_args.model_path,
            "model_config": runner.model_config.hf_config.to_dict(),
            "dtype": str(runner.model_config.dtype),
            "attention_backend": runner.server_args.attention_backend,
            "load_format": runner.server_args.load_format,
            "disaggregation_mode": runner.server_args.disaggregation_mode,
            "moe_a2a_backend": runner.server_args.moe_a2a_backend,
            "deepep_mode": runner.server_args.deepep_mode,
            "expert_transport": os.environ.get("DEEP_USE_ALLTOALL_MODE", "0"),
        }

        return result

    def record_batch(self, scheduler: Any, batch: Any, observation: str) -> int:
        from markov_collection.adapters.frameworks.sglang.batches import BatchTensors

        phase = "control"
        members = []
        if batch is not None:
            phase = "prefill" if batch.forward_mode.is_extend() else "decode"
            members = BatchTensors.members(batch)

        identity = len(self.recording.batches)
        self.recording.batches.append(
            {
                "id": identity,
                "phase": phase,
                "rank": self.parallel["rank"],
                "lane": self.batch_lane,
                "members": members,
                "parameters": {
                    "M": sum(item["new_tokens"] for item in members),
                    "B": len(members),
                    "R": scheduler.req_to_token_pool.size,
                    "slot_capacity": scheduler.req_to_token_pool.req_to_token.shape[0] - 1,
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

    def observation(self, requests: Any) -> str | None:
        if not self.selected:
            return None
        identities = {request.rid for request in requests}
        if identities and identities <= self.requests:
            return "source"
        if identities and identities <= self.baseline_requests:
            return "baseline"
        return None

    def begin_batch(self, scheduler: Any) -> None:
        self.operators.begin_batch()
        with self.operators.values.metadata():
            mapping = scheduler.req_to_token_pool.req_to_token
            shapes = self.operators.values.shapes

            # Pool capacity belongs to every value version, before preparation first reads or writes it.
            shapes.bind_shape(mapping, (shapes.symbol("slot_capacity") + 1, mapping.shape[1]))

    def schedule(self, original: Any, scheduler: Any) -> Any:
        if self.pipeline is not None:

            # The native loop installs its running view before this scheduling call can replace it.
            self.batch_lane = next(
                index for index, batch in enumerate(scheduler.running_mbs) if batch is scheduler.running_batch
            )
        pending = list(scheduler.waiting_queue)
        for batch in (scheduler.running_batch, scheduler.last_batch):
            if batch is not None:
                pending.extend(batch.reqs)

        # Finished members still participate in the native filter before the scheduler becomes idle.
        observation = self.observation(pending)
        if observation is None:
            return original(scheduler)

        identity = len(self.recording.batches)
        if observation == "source":
            self.begin_batch(scheduler)
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
                result = original(scheduler, batch, *args, **kwargs)
            if self.pipeline is not None:
                self.pipeline.remember(identity, result)
            return result

        # Decode relays tokens before the model wrapper binds inputs; serving batches are nonempty by contract.
        with self.recording.scope(phase.capitalize(), batch=identity) as scope, self.operators:
            with self.operators.values.metadata():
                for name in ("M", "B"):
                    self.operators.values.bounded_parameter(name, 1, sys.maxsize)
                for tensor in BatchTensors.read(batch).values():
                    self.operators.values.bind(tensor, {0: "B"})
                self.operators.values.bind_shape(
                    scheduler.future_map.output_tokens_buf, (self.operators.values.shapes.symbol("slot_capacity") + 1,)
                )

            fields = self.pipeline.fields if self.pipeline is not None and phase == "decode" else BatchTensors.fields
            scope.metadata["ports"] = {
                "inputs": self.operators.values.port_values(
                    {
                        **BatchPreparationCall.model_inputs(batch, fields),
                        "token_relay": scheduler.future_map.output_tokens_buf,
                    }
                ),
                "outputs": {},
            }

            # Decode contributes one token per member, so its token axis is B by definition.
            token_axis = "B" if phase == "decode" else "M"
            member_axis = "B"
            if self.ep_size > 1:
                from markov_collection.adapters.frameworks.sglang.expert_geometry import ExpertModelGeometry

                with self.operators.values.metadata():
                    ExpertModelGeometry.bind_batch(self.operators.values, token_axis, member_axis, self.ep_size)
                token_axis, member_axis = "model_tokens", "model_members"
            proxy = args[0] if args else kwargs.get("pp_proxy_tensors")
            if proxy is not None:
                payload = self.pipeline.payload(proxy.tensors)
                with self.operators.values.metadata():
                    for name, tensor in payload.items():
                        axis = f"proxy_{name}_tokens"
                        scope.metadata.setdefault("parameters", {})[axis] = tensor.shape[0]
                        self.operators.values.bounded_parameter(axis, 1, sys.maxsize)

                        # The previous stage supplies this geometry; EP can forward a local token shard.
                        self.operators.values.bind(tensor, {0: axis})
                    scope.metadata["ports"]["inputs"].update(
                        self.operators.values.port_values({f"proxy.{name}": tensor for name, tensor in payload.items()})
                    )
            inputs = {
                "input_ids": {0: token_axis},
                "positions": {0: token_axis},
                "forward_batch.out_cache_loc": {0: token_axis},
                "forward_batch.seq_lens": {0: member_axis},
                "forward_batch.req_pool_indices": {0: member_axis},
                "forward_batch.extend_seq_lens": {0: member_axis},
            }
            with ModuleObservation(
                self.recording,
                self.model,
                self.operators.values,
                inputs,
                {RadixAttention: AttentionCall, **self.experts.module_calls(self.model)},
            ):
                result = original(scheduler, batch, *args, **kwargs)

            scope.metadata["ports"]["outputs"] = self.operators.values.port_values(
                {
                    **BatchTensors.read(batch),
                    "token_relay": scheduler.future_map.output_tokens_buf,
                }
            )
            if result.next_token_ids is not None:
                scope.metadata["ports"]["outputs"].update(
                    self.operators.values.port_values({"sampled_tokens": result.next_token_ids})
                )
            if self.pipeline is not None:
                self.pipeline.remember(identity, result)
                if result.pp_hidden_states_proxy_tensors is not None:
                    scope.metadata["ports"]["outputs"].update(
                        self.operators.values.port_values(
                            {
                                f"proxy.{name}": tensor
                                for name, tensor in result.pp_hidden_states_proxy_tensors.tensors.items()
                            }
                        )
                    )
            return result

    def forward(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        import torch

        # Warmup discovers the context actually used by the model, including native PP's forward stream.
        self.model_stream = torch.npu.current_stream()
        if self.allocation is None:
            with self.matrices:
                return original(*args, **kwargs)
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
            scope.metadata["shape_contracts"] = [
                {"value": scope.metadata["ports"]["inputs"]["sampled_tokens"], "shape": ["B"]}
            ]

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

        # A prefill server acknowledges ownership transfer; only decode publishes user tokens.
        if (
            self.disaggregation is not None
            and self.disaggregation.role == "prefill"
            and isinstance(output, BatchTokenIDOutput)
            and set(output.rids) <= self.requests
            and output.rids
        ):
            with self.recording.scope("KvAcknowledged", requests=list(output.rids)):
                return original(sender, output, *args, **kwargs)

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
            values.bind_shape(relay.output_tokens_buf, (values.shapes.symbol("slot_capacity") + 1,))

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
            if self.disaggregation is not None and request.rid in self.requests:
                return self.disaggregation.release(original, cache, request, *args, **kwargs)
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
            import torch

            from markov_collection.adapters.frameworks.sglang.allocation import SGLangAllocation
            from markov_collection.adapters.frameworks.sglang.primitives import SGLangPrimitives
            from markov_collection.adapters.runtimes.ascend.materialization import AscendMaterializations

            if self.allocation is None:
                pool = scheduler.token_to_kv_pool_allocator
                self.allocation = SGLangAllocation(self.operators, pool.page_size, str(pool.device), pool.need_sort)
                self.indexed_writes.request_map(scheduler.req_to_token_pool.req_to_token)
                self.materializations = AscendMaterializations(
                    self.operators,
                    pool.page_size,
                    scheduler.req_to_token_pool.req_to_token.dtype,
                    ("B", "max_sequence_tokens"),

                    # Prefill members remain unpadded; Decode follows native MLP token alignment.
                    (1, self.ep_size),
                )
                with torch.npu.stream(self.model_stream):
                    self.matrices.prepare()
                self.workspace_policy = self.workspaces.reserve(
                    self.matrices.description["cube_cores"],
                    SGLangPrimitives.attention_geometries(self.model),
                    [torch.npu.current_stream().npu_stream, self.model_stream.npu_stream],
                )
            self.concatenation.warmup()
            self.allocation.warmup()
            with torch.npu.stream(self.model_stream):
                self.materializations.warmup()
            self.indexed_writes.warmup()

            boundary = self.recording.batches[-1]["observation"] if self.recording.batches else "warmup"
            self.workspace_states[boundary] = self.workspaces.snapshot()

            if any(batch["observation"] == "source" for batch in self.recording.batches):
                self.concatenation.capture()
                self.allocation.capture()
                with torch.npu.stream(self.model_stream):
                    self.matrices.capture()
                    self.materializations.capture()
                self.indexed_writes.capture()
        if result and self.disaggregation is not None and self.derived_role:
            write_json(self.directory / f"rank-{self.parallel['rank']}-layout.json", self.parallel["kv_layout"])
        if result and any(batch["observation"] == "source" for batch in self.recording.batches):
            if self.pipeline is None:
                self.write()
            else:
                self.pipeline.export(scheduler)

        return result

    def write(self) -> None:
        import torch
        import torch_npu

        self.recording.write(
            self.directory / f"rank-{self.parallel['rank']}.json",
            {
                "framework": "sglang",
                "torch": torch.__version__,
                "torch_npu": torch_npu.__version__,
                "cann": self.concatenation.version,
                "matrix_selection": self.matrices.description,
                "observer": __file__,
                **self.parallel,
                "source_transport": "native_disaggregation"
                if self.disaggregation is not None
                else "native_process_groups"
                if self.process_groups is not None
                else "physical_group_of_one",
                "implementation": self.implementation,
                "device": torch.npu.get_device_name(),
                "device_index": torch.npu.current_device(),
                "workspace_boundaries": self.workspace_states,
                "workspace_policy": self.workspace_policy,
            },
        )
        self.synchronization.write(self.directory / "runtime")

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        from markov_collection.adapters.frameworks.sglang.batches import BatchTensors
        from markov_collection.adapters.frameworks.sglang.preparation import SGLangPreparation
        from markov_collection.adapters.frameworks.sglang.primitives import SGLangPrimitives
        from markov_collection.adapters.runtimes.ascend.functions import AscendFunctionObservation
        from markov_collection.adapters.runtimes.ascend.trace import AscendTrace

        functions = AscendFunctionObservation(self.operators.values)
        SGLangPrimitives(self.operators.values).install()
        self.experts.install()
        fields = BatchTensors.fields

        # PP keeps returned tokens in the batch until its next model invocation.
        if self.pipeline is not None:
            fields = self.pipeline.fields
        SGLangPreparation(self.operators.values, fields, self.ep_size > 1).install()
        if self.ep_size > 1:
            from markov_collection.adapters.frameworks.sglang.expert_geometry import ExpertModelGeometry

            ExpertModelGeometry(self.operators.values).install()

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
            "torch_npu.profiler.tensorboard_trace_handler": AscendTrace.capture_handler,
            functions.target: functions.split_qkv,
        }
        if self.disaggregation is not None:
            targets.pop("sglang.srt.managers.scheduler.Scheduler.get_next_batch_to_run")
            role = self.disaggregation.role

            # Native PP Prefill calls admission directly; the non-PP loop owns an outer PD scheduling method.
            method = (
                "get_new_batch_prefill"
                if self.pipeline is not None and role == "prefill"
                else f"get_next_disagg_{role}_batch_to_run"
            )
            target = f"sglang.srt.managers.scheduler.Scheduler.{method}"
            targets[target] = self.schedule
        for target, observer in targets.items():
            HookRegistry.register(target, observer, HookType.AROUND)

        if self.routing is not None:
            self.routing.install()

        if self.shard is not None:
            self.shard.install()
        if self.process_groups is not None:
            self.process_groups.install()
        if self.pipeline is not None:
            self.pipeline.install()
        if self.disaggregation is not None:
            self.disaggregation.install()


def install() -> None:
    description = os.environ.get("MARKOV_GRAPH_SESSION")
    if description is not None:
        SGLangObservation(json.loads(Path(description).read_text())).install()
