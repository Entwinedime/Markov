"""Observe the official non-overlap SGLang path through its native plugin hooks."""

from __future__ import annotations

import inspect
import os
import sys
from collections.abc import Callable
from pathlib import Path
from typing import Any

from markov_collection.adapters.frameworks.sglang.batch_observer import SGLangProfileObserver
from markov_collection.adapters.frameworks.sglang.input_broadcast import NativeInputMessage, SGLangInputBroadcast
from markov_collection.artifacts.json_file import write_json
from markov_collection.instrumentation.blocks import BlockWrapper, BoundBlock
from markov_collection.instrumentation.native_regions import BoundNativeRegion, NativeRegions, NativeRegionWrapper
from markov_collection.instrumentation.recording import Observation, Recorder


class SGLangObserver(SGLangProfileObserver):
    def __init__(
        self,
        directory: Path,
        request_ids: set[str],
        regions: dict[str, Any],
        *,
        capture_cleanup: bool = False,
        source_rank: int | None = None,
    ) -> None:
        super().__init__(directory, request_ids, source_rank=source_rank)
        self.capture_cleanup = capture_cleanup
        self._cleaned_ids: set[str] = set()
        self._schedule_filters: list[dict[str, list[str]]] | None = None
        self._regions = NativeRegions(regions)

    def forward(self, original: Callable[..., Any], worker: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(worker, batch, *args, **kwargs)

        with self.markers.observer():
            # Installing wrappers is collection work, outside the model body.
            phase = "prefill" if batch.forward_mode.is_extend() else "decode"
            metadata = self._batch(batch, phase)

            self._regions.install(worker.model_runner.model, self.recorder)
            self._regions.install_components(worker.model_runner, self.recorder, "model_runner")
            observation = self.recorder.scope("model_forward", metadata)

        with observation:
            return original(worker, batch, *args, **kwargs)


class SGLangBlockWrapper(BlockWrapper):
    def __init__(
        self, observer: SGLangObserver, definitions: dict[str, dict[str, Any]], bindings: list[dict[str, Any]]
    ) -> None:
        blocks = tuple(
            BoundBlock(
                binding["id"],
                binding["step"],
                binding["phase"],
                tuple(parameter["name"] for parameter in definitions[binding["step"]]["parameters"]),
            )
            for binding in bindings
        )
        super().__init__(blocks, observer.markers)
        self.observer = observer


class SGLangScheduleWrapper(SGLangBlockWrapper):
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self.scheduler: Any = None
        self.pending: Observation | None = None
        self.awaiting_idle = False

    def context(self, scheduler: Any, *args: Any, **kwargs: Any) -> tuple[Recorder, dict[str, Any]] | None:
        visible = self.observer._visible_requests(scheduler)
        # Base discovery retains native cleanup at group boundaries. Target
        # validation ends its observation at the last publication.
        finished = self.observer._finished_ids == self.observer.request_ids
        if self.observer._cleaned_ids == self.observer.request_ids or (finished and not self.observer.capture_cleanup):
            return None
        self.observer._schedule_filters = []
        from sglang.srt.environ import envs
        from sglang.srt.managers import scheduler as scheduler_module
        from sglang.srt.managers.schedule_policy import CLIP_MAX_NEW_TOKENS, IGNORE_EOS_RESERVE_TOKENS

        # Warmup may already have decayed the worker's ratio. Keep the observed
        # entry value separate from the policy parameters used by a target run.
        ratio = scheduler.new_token_ratio_tracker
        receiver = scheduler.request_receiver
        return self.observer._recorder(scheduler.ps.tp_rank), {
            "visible_requests": [req.rid for req in visible],
            # Read the worker and loaded module, not launcher environment defaults.
            "worker_controls": {
                "physical_tp_size": scheduler.tp_worker.tp_size,
                "physical_tp_rank": scheduler.tp_worker.tp_rank,
                "scheduler_tp_size": scheduler.ps.tp_size,
                "scheduler_tp_rank": scheduler.ps.tp_rank,
                "receiver_tp_size": receiver.ps.tp_size,
                "receiver_tp_rank": receiver.ps.tp_rank,
                "waiting_timeout_seconds": envs.SGLANG_REQ_WAITING_TIMEOUT.get(),
                "running_timeout_seconds": envs.SGLANG_REQ_RUNNING_TIMEOUT.get(),
                "test_retract": scheduler_module.TEST_RETRACT,
                "require_mlp_sync": scheduler.require_mlp_sync,
                "max_recv_per_poll": receiver.max_recv_per_poll,
                "recv_skipper": receiver.recv_skipper is not None,
                "input_blocker": receiver.input_blocker is not None,
                "scripted_scheduler_hook": receiver.scripted_scheduler_hook is not None,
                "idle_sleeper": scheduler.idle_sleeper is not None,
            },
            "admission_policy": {
                "initial_ratio": ratio.init,
                "minimum_ratio": ratio.min,
                "decay_per_decode": ratio.decay,
                "entry_ratio": ratio.current,
                "output_estimate_limit_tokens": CLIP_MAX_NEW_TOKENS,
                "reserve_tokens_per_request": IGNORE_EOS_RESERVE_TOKENS,
            },
        }

    def receive(self, original: Callable[..., Any], receiver: Any, *args: Any, **kwargs: Any) -> Any:
        with self.markers.observer():
            context = None
            if self.scheduler is not None and self.observer.recorder is not None:
                context = self.context(self.scheduler)
            if context is not None:
                recorder, metadata = context
                self.pending = recorder.scope("schedule", metadata, binding=self.blocks[0].identity, pending=True)
                self.pending.result.update(input_requests=[], request_ready=[], input_ready=None, input_supported=True)
                if any(rid in self.observer.request_ids for rid in metadata["visible_requests"]):
                    self.pending.commit()
        if self.pending is None:
            return original(receiver, *args, **kwargs)

        self.pending.__enter__()
        try:
            result = original(receiver, *args, **kwargs)
            with self.markers.observer():
                incoming, supported = self.observer._input_requests(result)
                self.pending.result["input_requests"] = incoming
                self.pending.result["input_supported"] = supported
                if any(request["id"] in self.observer.request_ids for request in incoming):
                    self.pending.commit()
            return result
        except BaseException:
            self._close(sys.exc_info())
            raise

    def process(self, original: Callable[..., Any], scheduler: Any, *args: Any, **kwargs: Any) -> Any:
        self.scheduler = scheduler
        try:
            result = original(scheduler, *args, **kwargs)
            with self.markers.observer():
                if self.pending is not None:
                    self.pending.result["input_supported"] &= self.observer._inputs_accepted(
                        scheduler, self.pending.result["input_requests"]
                    )
            return result
        except BaseException:
            self._close(sys.exc_info())
            raise

    def invoke(self, original: Callable[..., Any], scheduler: Any, *args: Any, **kwargs: Any) -> Any:
        if self.pending is None:
            return original(scheduler, *args, **kwargs)
        with self.markers.observer():
            self.pending.metadata["visible_requests"] = [req.rid for req in self.observer._visible_requests(scheduler)]
        try:
            result = original(scheduler, *args, **kwargs)
            with self.markers.observer():
                self.completed(self.pending, result, scheduler, *args, **kwargs)
                self.awaiting_idle = result is None
            if result is not None:
                self._close()
            return result
        except BaseException:
            self._close(sys.exc_info())
            raise

    def idle(self, original: Callable[..., Any], scheduler: Any, *args: Any, **kwargs: Any) -> Any:
        try:
            result = original(scheduler, *args, **kwargs)
            if self.awaiting_idle:
                self._close()
            return result
        except BaseException:
            self._close(sys.exc_info())
            raise

    def _close(self, error: tuple[Any, Any, Any] = (None, None, None)) -> None:
        observation = self.pending
        self.pending = None
        self.awaiting_idle = False
        self.observer._schedule_filters = None
        if observation is not None:
            observation.__exit__(*error)
            if observation.committed:
                with self.markers.observer():
                    self.closed(observation)

    def completed(self, observation: Observation, result: Any, *args: Any, **kwargs: Any) -> None:
        observation.result["batch_id"] = self.observer._batch_ids[id(result)] if result is not None else None
        observation.result["batch_filters"] = self.observer._schedule_filters
        self.observer._schedule_filters = None

    def closed(self, observation: Observation) -> None:
        if not self.observer.capture_cleanup or self.observer._finished_ids == self.observer._cleaned_ids:
            return
        if observation.result["batch_id"] is not None:
            return
        if any(rid not in self.observer._finished_ids for rid in observation.metadata["visible_requests"]):
            return

        # Publish the acknowledgement after the actual Schedule record and all
        # nested cleanup observations have been persisted, never from a response.
        recorder = self.observer.recorder
        final = self.observer._finished_ids == self.observer.request_ids
        recorder.flush_native(close_window=final)
        self.observer._cleaned_ids = self.observer._finished_ids.copy()
        write_json(
            self.observer.directory / f"rank-{recorder.rank}-cleanup.json",
            {
                "rank": recorder.rank,
                "source_call": observation.identity,
                "completion": "native_empty_schedule",
                "request_ids": sorted(self.observer._cleaned_ids),
            },
        )


class SGLangBatchWrapper(BlockWrapper):
    def __init__(self, observer: SGLangObserver, contract: dict[str, Any], binding: dict[str, Any]) -> None:
        parameters = tuple(parameter["name"] for parameter in contract["definition"]["parameters"])
        super().__init__((BoundBlock(binding["id"], binding["step"], binding["phase"], parameters),), observer.markers)
        self.observer = observer
        self.binding = binding

    def context(self, batch: Any, *args: Any, **kwargs: Any) -> tuple[Recorder, dict[str, Any]] | None:
        filters = self.observer._schedule_filters
        if filters is None:
            return None

        # Filter identities include no-ops; a merge records its position between
        # these calls and its input sizes without another mutable request list.
        if self.binding["semantic"] == "batch_filter":
            index = len(filters)
            filters.append({"before": [req.rid for req in batch.reqs], "after": []})
            parameters = {"filter_index": index}
        else:
            other = args[0] if args else kwargs["other"]
            parameters = {
                "filter_prefix_count": len(filters),
                "left_requests": len(batch.reqs),
                "right_requests": len(other.reqs),
            }

        return self.observer.recorder, {
            "module_type": self.binding["module_type"],
            "method": self.binding["method"],
            "module_parameters": {},
            "call_parameters": parameters,
            "inputs": {},
            "outputs": {},
            "cpu_values": {},
            "request_id": None,
            "group_ranks": None,
        }

    def completed(self, observation: Observation, result: Any, batch: Any, *args: Any, **kwargs: Any) -> None:
        if self.binding["semantic"] != "batch_filter":
            return

        index = observation.metadata["call_parameters"]["filter_index"]
        self.observer._schedule_filters[index]["after"] = [req.rid for req in batch.reqs]


class SGLangPrepareWrapper(SGLangBlockWrapper):
    def context(self, batch: Any, *args: Any, **kwargs: Any) -> tuple[Recorder, dict[str, Any]] | None:
        if not self.observer._tracked(batch.reqs):
            return None

        self.observer._batch_sequence += 1
        self.observer._batch_ids[id(batch)] = self.observer._batch_sequence
        self.observer._regions.install_components(batch, self.observer.recorder, "batch")

        # The enclosing schedule scope establishes both the recorder and parent identity.
        return self.observer.recorder, self.observer._batch(batch, self.blocks[0].phase)

    def completed(self, observation: Observation, result: Any, batch: Any, *args: Any, **kwargs: Any) -> None:
        # Request slots are assigned during native preparation. Read the Python
        # request fields after that assignment without copying the device table.
        observation.result["request_pool_slots"] = [
            {"request_id": request.rid, "pool_slot": request.req_pool_idx} for request in batch.reqs
        ]


class SGLangExecutionWrapper(SGLangBlockWrapper):
    def context(self, scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> tuple[Recorder, dict[str, Any]] | None:
        if not self.observer._tracked(batch.reqs):
            return None

        phase = "prefill" if batch.forward_mode.is_extend() else "decode"
        return self.observer._recorder(scheduler.ps.tp_rank), self.observer._batch(batch, phase)


class SGLangPublishWrapper(SGLangExecutionWrapper):
    def completed(
        self, observation: Observation, result: Any, scheduler: Any, batch: Any, *args: Any, **kwargs: Any
    ) -> None:
        from sglang.srt.managers.scheduler_components.output_streamer import DEFAULT_FORCE_STREAM_INTERVAL

        # Read the worker module's effective policy, not the parent's environment
        # or a copied default. This fact does not qualify a target transformation.
        observation.result["output_policy"] = {
            "force_interval_tokens": DEFAULT_FORCE_STREAM_INTERVAL,
            "non_streaming": all(not req.stream for req in batch.reqs),
        }
        observation.result["requests"] = [
            {"id": req.rid, "generated_tokens": len(req.output_ids), "finished": req.finished()} for req in batch.reqs
        ]

    def closed(self, observation: Observation) -> None:
        for request in observation.result["requests"]:
            if request["finished"] and request["id"] in self.observer.request_ids:
                self.observer._finished_ids.add(request["id"])

        if self.observer._finished_ids == self.observer.request_ids and not self.observer.capture_cleanup:
            self.observer.recorder.flush_native(close_window=True)


class SGLangOutputWrapper(NativeRegionWrapper):
    def metadata(self, arguments: inspect.BoundArguments) -> dict[str, Any]:
        metadata = super().metadata(arguments)

        from sglang.srt.managers.schedule_batch import INIT_INCREMENTAL_DETOKENIZATION_OFFSET
        from sglang.srt.managers.scheduler_components.output_streamer import DEFAULT_FORCE_STREAM_INTERVAL

        self._requests = arguments.arguments["reqs"]
        skipped = arguments.arguments["skip_req"]
        self._output_work = {
            "decode_context_limit": INIT_INCREMENTAL_DETOKENIZATION_OFFSET,
            "force_interval_tokens": DEFAULT_FORCE_STREAM_INTERVAL,
            "sender_enabled": self.module.send_to_detokenizer.socket is not None,
            "speculative": not self.module.spec_algorithm.is_none(),
            "disaggregation_mode": self.module.disaggregation_mode.value,
            "request_time_stats": self.module.server_args.enable_request_time_stats_logging,
            "before": [self.request_state(request, request is skipped) for request in self._requests],
        }
        return metadata

    @staticmethod
    def request_state(request: Any, skipped: bool) -> dict[str, Any]:
        # Read scalar/list metadata only. Do not slice token lists, serialize a
        # second payload, or ask native detokenization to advance its state.
        return {
            "request_id": request.rid,
            "skipped": skipped,
            "prompt_tokens": len(request.origin_input_ids_unpadded),
            "generated_tokens": len(request.output_ids),
            "finished": request.finished(),
            "finished_output": request.finished_output,
            "finished_len": request.finished_len,
            "send_token_offset": request.send_token_offset,
            "send_decode_id_offset": request.send_decode_id_offset,
            "read_offset": request.read_offset,
            "surr_offset": request.surr_offset,
            "decoded_text_characters": len(request.decoded_text),
            "stream": request.stream,
            "return_hidden_states": request.return_hidden_states,
            "return_routed_experts": request.return_routed_experts,
            "return_indexer_topk": request.return_indexer_topk,
            "customized_info": request.customized_info is not None,
            "multimodal": request.multimodal_inputs is not None
            or bool(request.mm_image_tokens or request.mm_audio_tokens or request.mm_video_tokens),
        }

    def completed(self, observation: Observation, result: Any, *args: Any, **kwargs: Any) -> None:
        super().completed(observation, result, *args, **kwargs)
        # Each wrapper belongs to one invocation, so mutable request references
        # never escape the call. Preserve native before/after offsets verbatim.
        self._output_work["after"] = [
            self.request_state(request, before["skipped"])
            for request, before in zip(self._requests, self._output_work["before"], strict=True)
        ]
        observation.result["output_work"] = self._output_work


class SGLangInputResolutionWrapper(NativeRegionWrapper):
    def metadata(self, arguments: inspect.BoundArguments) -> dict[str, Any]:
        metadata = super().metadata(arguments)

        # Retain metadata, not the staging tensor: observation must not extend
        # its lifetime or change the asynchronous copy's ownership.
        tensor = self.module.prefill_input_ids_cpu
        self._input_copy = None
        if tensor is not None:
            self._input_copy = {
                "source_address": tensor.data_ptr(),
                "bytes": tensor.numel() * tensor.element_size(),
            }
        return metadata

    def completed(self, observation: Observation, result: Any, *args: Any, **kwargs: Any) -> None:
        super().completed(observation, result, *args, **kwargs)
        observation.result["input_copy"] = self._input_copy
        if self._input_copy is not None:
            self._input_copy["destination_address"] = self.module.input_ids.data_ptr()


class SGLangCollectiveWrapper(NativeRegionWrapper):
    def metadata(self, arguments: inspect.BoundArguments) -> dict[str, Any]:
        from sglang.srt.runtime_context import get_parallel

        parallel = get_parallel()
        parameters = {"dim": arguments.arguments["dim"]} if self.binding.method == "all_gather" else {}

        # Observe the same logical interface around native or substituted work.
        # Parent identity and layouts are facts; elapsed time is not a cost.
        return {
            "module_type": self.binding.module_type,
            "method": self.binding.method,
            "module_parameters": {
                "logical_tp_size": parallel.tp_size,
                "logical_rank": parallel.tp_rank,
                "physical_tp_size": self.module.world_size,
            },
            "group_ranks": list(self.module.ranks),
            "call_parameters": parameters,
            "inputs": {"input_": self.tensor_layout(arguments.arguments["input_"])},
            "outputs": {},
            "cpu_values": {},
            "request_id": None,
        }


class SGLangRegionHook:
    def __init__(
        self,
        observer: SGLangObserver,
        contract: dict[str, Any],
        binding: dict[str, Any],
        wrapper_type: type[NativeRegionWrapper],
    ) -> None:
        parameters = tuple(parameter["name"] for parameter in contract["definition"]["parameters"])
        self.binding = BoundNativeRegion.read(binding, parameters)
        self.observer = observer
        self.wrapper_type = wrapper_type

    def invoke(self, original: Callable[..., Any], component: Any, *args: Any, **kwargs: Any) -> Any:
        with self.observer.markers.observer():
            recorder = self.observer.recorder
            active = recorder is not None and recorder.in_scope()
            if active:
                # Retain the native hook-chain function; wrapper construction
                # observes its signature but never advances serving state.
                native = original.__get__(component, type(component))
                wrapper = self.wrapper_type(self.binding, component, recorder, native)

        if not active:
            return original(component, *args, **kwargs)

        return wrapper(*args, **kwargs)


class SGLangInputRegionHook(SGLangRegionHook):
    def __init__(
        self,
        observer: SGLangObserver,
        contract: dict[str, Any],
        binding: dict[str, Any],
        *,
        schedule: SGLangScheduleWrapper,
    ) -> None:
        super().__init__(observer, contract, binding, NativeRegionWrapper)
        self.semantic = binding["semantic"]
        self.schedule = schedule

    def invoke(self, original: Callable[..., Any], component: Any, *args: Any, **kwargs: Any) -> Any:
        with self.observer.markers.observer():
            recorder = self.observer.recorder
            active = self.schedule.pending is not None and recorder is not None
            if active:
                native = original.__get__(component, type(component))
                wrapper = NativeRegionWrapper(self.binding, component, recorder, native)
        if not active:
            return original(component, *args, **kwargs)

        with wrapper.scope(*args, **kwargs) as observation:
            result = native(*args, **kwargs)
            with self.observer.markers.observer():
                wrapper.completed(observation, result, *args, **kwargs)
                if self.semantic == "input_request":
                    self.schedule.pending.result["request_ready"].append(
                        {"id": observation.metadata["request_id"], "call_id": observation.identity}
                    )
                elif self.semantic == "input_process":
                    self.schedule.pending.result["input_ready"] = observation.identity
        return result


class SGLangInputMessageHook(BlockWrapper):
    def __init__(
        self,
        observer: SGLangObserver,
        contract: dict[str, Any],
        binding: dict[str, Any],
        channel: SGLangInputBroadcast,
    ) -> None:
        parameters = tuple(parameter["name"] for parameter in contract["definition"]["parameters"])
        self.binding = BoundNativeRegion.read(binding, (*parameters, "message"))
        super().__init__((self.binding,), observer.markers)
        self.observer = observer
        self.channel = channel
        self.semantic = binding["semantic"]

    def context(self, call: NativeInputMessage) -> tuple[Recorder, dict[str, Any]]:
        return self.observer.recorder, {
            "module_type": self.binding.module_type,
            "method": self.binding.method,
            "module_parameters": {},
            "call_parameters": {},
            "inputs": {},
            "outputs": {},
            "cpu_values": {},
            "request_id": None,
            "group_ranks": call.message["group_ranks"],
            "message": call.message,
        }

    def invoke(self, original: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
        with self.markers.observer():
            recorder = self.observer.recorder
            call = (
                self.channel.describe(self.semantic, original, *args, **kwargs)
                if recorder is not None and recorder.in_scope()
                else None
            )
        if call is None:
            return original(*args, **kwargs)

        # Both child ranges belong to this actual synchronous call. Submission
        # return bounds enqueue completion; it is not a worker-start timestamp.
        with self.scope(call) as observation:
            result = self.channel.invoke_message(self.semantic, call, original, *args, **kwargs)
            if self.semantic == "input_message_submit":
                with self.markers.observer():
                    observation.result["returns_work"] = call.work is not None
        return result


def install(prepared: dict[str, Any]) -> None:
    from sglang.srt.plugins.hook_registry import HookRegistry, HookType

    observer = SGLangObserver(
        Path(os.environ["MARKOV_OBSERVATIONS"]),
        {request["id"] for request in prepared["requests"]},
        prepared["native_regions"],
        capture_cleanup=prepared["role"] == "base",
        source_rank=prepared.get("shard_execution", {}).get("rank"),
    )

    # Inline locations are shared with independent CPU clocks. Only this
    # observation path reads semantic parameters and adds native work counters.
    from markov_collection.adapters.frameworks.sglang.inline import SGLangInlineMethod, SGLangInlineObservation

    regions = prepared["native_regions"]
    input_broadcast = next(binding for binding in regions["bindings"] if binding["semantic"] == "input_broadcast")
    channel = SGLangInputBroadcast(input_broadcast["id"], observer.markers)
    for hook, method_regions in SGLangInlineMethod.group(regions).items():
        scopes = {region.binding["id"]: SGLangInlineObservation(observer, regions, region) for region in method_regions}
        method = SGLangInlineMethod(method_regions, observer.markers, scopes, record_work=True)
        HookRegistry.register(hook, method.invoke, HookType.AROUND)

    definitions = {definition["step"]: definition for definition in prepared["blocks"]}
    wrappers = {
        "schedule": SGLangScheduleWrapper,
        "prepare": SGLangPrepareWrapper,
        "execute": SGLangExecutionWrapper,
        "publish": SGLangPublishWrapper,
    }
    hooks: dict[str, list[dict[str, Any]]] = {}
    for binding in prepared["block_bindings"]:
        hooks.setdefault(binding["hook"], []).append(binding)

    # One native hook may implement several phase-specific standard blocks.
    schedule = None
    schedule_hook = None
    for hook, bindings in hooks.items():
        wrapper = wrappers[bindings[0]["wrapper"]](observer, definitions, bindings)
        if bindings[0]["wrapper"] == "schedule":
            schedule = wrapper
            schedule_hook = hook
        else:
            HookRegistry.register(hook, wrapper.invoke, HookType.AROUND)

    region_wrappers = {
        "batch_output": SGLangOutputWrapper,
        "input_resolution": SGLangInputResolutionWrapper,
        "logical_collective": SGLangCollectiveWrapper,
    }
    input_semantics = {
        "input_receive",
        "input_pull",
        "input_broadcast",
        "input_process",
        "input_request",
        "idle_suffix",
    }
    for binding in regions["bindings"]:
        if binding["semantic"] in input_semantics:
            wrapper = SGLangInputRegionHook(observer, regions, binding, schedule=schedule)
            HookRegistry.register(binding["hook"], wrapper.invoke, HookType.AROUND)
        elif binding["semantic"] in {"input_message", "input_message_submit", "input_message_wait"}:
            wrapper = SGLangInputMessageHook(observer, regions, binding, channel)
            HookRegistry.register(binding["hook"], wrapper.invoke, HookType.AROUND)
        elif binding["semantic"] in region_wrappers:
            wrapper = SGLangRegionHook(observer, regions, binding, region_wrappers[binding["semantic"]])
            HookRegistry.register(binding["hook"], wrapper.invoke, HookType.AROUND)
        elif binding["semantic"] in {"batch_filter", "batch_merge"}:
            wrapper = SGLangBatchWrapper(observer, regions, binding)
            HookRegistry.register(binding["hook"], wrapper.invoke, HookType.AROUND)

    HookRegistry.register(input_broadcast["hook"], channel.invoke, HookType.AROUND)
    scheduler = "sglang.srt.managers.scheduler.Scheduler"
    HookRegistry.register(schedule_hook, schedule.receive, HookType.AROUND)
    HookRegistry.register(f"{scheduler}.process_input_requests", schedule.process, HookType.AROUND)
    HookRegistry.register(f"{scheduler}.get_next_batch_to_run", schedule.invoke, HookType.AROUND)
    HookRegistry.register(f"{scheduler}.on_idle", schedule.idle, HookType.AROUND)

    HookRegistry.register(
        "sglang.srt.managers.tp_worker.TpModelWorker.forward_batch_generation", observer.forward, HookType.AROUND
    )
    observer.install_profile_hooks()
