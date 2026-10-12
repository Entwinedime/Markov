"""Observe the official non-overlap SGLang path through its native plugin hooks."""

from __future__ import annotations

import json
import os
from collections.abc import Callable
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.torch_npu.profiling import AscendProfileWindow
from markov_collection.adapters.runtimes.pytorch.profiling import TorchProfilerMarkers
from markov_collection.artifacts.json_file import write_json
from markov_collection.instrumentation.blocks import BlockWrapper, BoundBlock
from markov_collection.instrumentation.native_regions import BoundNativeRegion, NativeRegions, NativeRegionWrapper
from markov_collection.instrumentation.recording import Observation, Recorder


class SGLangObserver:
    def __init__(
        self, directory: Path, request_ids: set[str], regions: dict[str, Any], *, capture_cleanup: bool = False
    ) -> None:
        self.directory = directory
        self.request_ids = request_ids
        self.recorder: Recorder | None = None
        self.profile = AscendProfileWindow(self.directory.parent / "workspace_states")
        self._batch_sequence = 0
        self._batch_ids: dict[int, int] = {}
        self._finished_ids: set[str] = set()
        self.capture_cleanup = capture_cleanup
        self._cleaned_ids: set[str] = set()
        self._schedule_filters: list[dict[str, list[str]]] | None = None
        self._regions = NativeRegions(regions)

    def _recorder(self, rank: int) -> Recorder:
        if self.recorder is None:
            self.recorder = Recorder(self.directory, rank, TorchProfilerMarkers())

        return self.recorder

    def _tracked(self, reqs: list[Any]) -> bool:
        return any(req.rid in self.request_ids and not req.finished() for req in reqs)

    def _batch(self, batch: Any, phase: str) -> dict[str, Any]:
        requests = []
        for req in batch.reqs:
            prompt_tokens = len(req.origin_input_ids)
            generated_tokens = len(req.output_ids)
            requests.append(
                {
                    "id": req.rid,
                    "input_tokens": req.extend_input_len if phase == "prefill" else 1,
                    "context_tokens": len(req.prefix_indices)
                    if phase == "prefill"
                    else prompt_tokens + generated_tokens - 1,
                    "prompt_tokens": prompt_tokens,
                    "generated_tokens": generated_tokens,
                }
            )

        return {"batch_id": self._batch_ids[id(batch)], "phase": phase, "requests": requests}

    def forward(self, original: Callable[..., Any], worker: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(worker, batch, *args, **kwargs)

        # Current SGLang defers input H2D/gather to resolve_forward_inputs. The
        # device tensor exists here, rather than at prepare_for_extend/decode return.
        phase = "prefill" if batch.forward_mode.is_extend() else "decode"
        metadata = self._batch(batch, phase)
        metadata.update(
            input_shape=list(batch.input_ids.shape),
            input_dtype=str(batch.input_ids.dtype),
            input_stride=list(batch.input_ids.stride()),
            input_device=str(batch.input_ids.device),
        )

        self._regions.install(worker.model_runner.model, self.recorder)
        self._regions.install_components(worker.model_runner, self.recorder, "model_runner")
        with self.recorder.scope("model_forward", metadata):
            return original(worker, batch, *args, **kwargs)

    def start_profile(self, original: Callable[..., Any], manager: Any, *args: Any, **kwargs: Any) -> Any:
        self.profile.begin(manager.ps.tp_rank)
        # Observe allocator changes during profiler startup as well as requests.
        # Warmup state is read above; starting a journal must not imply an empty cache.
        self._recorder(manager.ps.tp_rank)

        with self.profile.starting():
            return original(manager, *args, **kwargs)

    def stop_profile(self, original: Callable[..., Any], manager: Any, *args: Any, **kwargs: Any) -> Any:
        with self.profile.stopping():
            result = original(manager, *args, **kwargs)
        if self.recorder is not None:
            self.recorder.flush_native(close_window=True)
        self.profile.end()

        return result


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
        super().__init__(blocks)
        self.observer = observer


class SGLangScheduleWrapper(SGLangBlockWrapper):
    def context(self, scheduler: Any, *args: Any, **kwargs: Any) -> tuple[Recorder, dict[str, Any]] | None:
        visible = scheduler.waiting_queue + scheduler.running_batch.reqs
        if scheduler.last_batch is not None:
            visible = visible + scheduler.last_batch.reqs
        # Base discovery retains native cleanup at group boundaries. Validation and
        # independent sampling retain their last-publication scope.
        finished = self.observer._finished_ids == self.observer.request_ids
        if self.observer._cleaned_ids == self.observer.request_ids or (finished and not self.observer.capture_cleanup):
            return None
        if not any(req.rid in self.observer.request_ids for req in visible):
            return None

        self.observer._schedule_filters = []
        from sglang.srt.environ import envs
        from sglang.srt.managers import scheduler as scheduler_module
        from sglang.srt.managers.schedule_policy import CLIP_MAX_NEW_TOKENS, IGNORE_EOS_RESERVE_TOKENS

        # Warmup may already have decayed the worker's ratio. Keep the observed
        # entry value separate from the policy parameters used by a target run.
        ratio = scheduler.new_token_ratio_tracker
        return self.observer._recorder(scheduler.ps.tp_rank), {
            "visible_requests": [req.rid for req in visible],
            # Read the worker and loaded module, not launcher environment defaults.
            "worker_controls": {
                "waiting_timeout_seconds": envs.SGLANG_REQ_WAITING_TIMEOUT.get(),
                "running_timeout_seconds": envs.SGLANG_REQ_RUNNING_TIMEOUT.get(),
                "test_retract": scheduler_module.TEST_RETRACT,
                "require_mlp_sync": scheduler.require_mlp_sync,
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
        super().__init__((BoundBlock(binding["id"], binding["step"], binding["phase"], parameters),))
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
            "module_path": self.binding["method"] + ":self",
            "module_type": self.binding["module_type"],
            "method": self.binding["method"],
            "module_parameters": {},
            "call_parameters": parameters,
            "inputs": {},
            "outputs": {},
            "cpu_values": {},
            "request_id": None,
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
    def context(self, *args: Any, **kwargs: Any) -> tuple[Recorder, dict[str, Any]] | None:
        context = super().context(*args, **kwargs)
        if context is None:
            return None

        from sglang.srt.managers.schedule_batch import INIT_INCREMENTAL_DETOKENIZATION_OFFSET
        from sglang.srt.managers.scheduler_components.output_streamer import DEFAULT_FORCE_STREAM_INTERVAL

        arguments = self.signature.bind(*args, **kwargs)
        arguments.apply_defaults()
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
        return context

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
    def context(self, *args: Any, **kwargs: Any) -> tuple[Recorder, dict[str, Any]] | None:
        context = super().context(*args, **kwargs)
        if context is None:
            return None

        # Retain metadata, not the staging tensor: observation must not extend
        # its lifetime or change the asynchronous copy's ownership.
        tensor = self.module.prefill_input_ids_cpu
        self._input_copy = None
        if tensor is not None:
            self._input_copy = {
                "source_address": tensor.data_ptr(),
                "source_is_pinned": tensor.is_pinned(),
                "bytes": tensor.numel() * tensor.element_size(),
            }
        return context

    def completed(self, observation: Observation, result: Any, *args: Any, **kwargs: Any) -> None:
        super().completed(observation, result, *args, **kwargs)
        observation.result["input_copy"] = self._input_copy
        observation.result["input_ids"] = self.tensor_layout(self.module.input_ids)
        if self._input_copy is not None:
            self._input_copy["destination_address"] = self.module.input_ids.data_ptr()


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
        recorder = self.observer.recorder
        if recorder is None or not recorder.in_scope():
            return original(component, *args, **kwargs)

        # Bind the native function supplied by the hook chain, not the patched
        # class attribute. Keep invocation state local and leave slots untouched.
        native = original.__get__(component, type(component))
        wrapper = self.wrapper_type(self.binding, self.binding.identity, component, recorder, native)
        return wrapper(*args, **kwargs)


def install() -> None:
    prepared_input = os.environ.get("MARKOV_PREPARED_INPUT")
    if prepared_input is None:
        return

    # SGLang loads this entry point in each worker before starting its scheduler.
    # Hooks only observe native calls; third_party and installed source stay intact.
    from sglang.srt.plugins.hook_registry import HookRegistry, HookType

    with Path(prepared_input).open(encoding="utf-8") as stream:
        prepared = json.load(stream)

    cpu_sampling = prepared["role"] == "performance" and prepared["measurement_kind"] == "cpu"
    if cpu_sampling:
        from markov_collection.adapters.frameworks.sglang.cpu_sampling import CpuSamplingObserver

        observer = CpuSamplingObserver(
            Path(os.environ["MARKOV_CPU_SAMPLES"]),
            set(prepared["measurement_ids"]),
            set(prepared["warmup_ids"]),
            prepared["native_regions"],
        )
    elif prepared["role"] in {"base", "validation"}:
        observer = SGLangObserver(
            Path(os.environ["MARKOV_OBSERVATIONS"]),
            {request["id"] for request in prepared["requests"]},
            prepared["native_regions"],
            capture_cleanup=prepared["role"] == "base",
        )
    else:
        observer = SGLangObserver(
            Path(os.environ["MARKOV_OBSERVATIONS"]),
            set(prepared["measurement_ids"]) | set(prepared["warmup_ids"]),
            prepared["native_regions"],
        )

    if not cpu_sampling:
        # Inline hooks must be inside the serving wrapper on a shared target.
        from markov_collection.adapters.frameworks.sglang.inline import (
            SGLangAdmissionBudget,
            SGLangAdmissionPrefix,
            SGLangBatchTransition,
            SGLangDecodeUpdatePrefix,
            SGLangInlineMethod,
            SGLangLastLocation,
            SGLangLastLocations,
            SGLangPrefillStatistics,
            SGLangRequestPublication,
        )

        regions = prepared["native_regions"]
        inline_regions = {
            "batch_transition": SGLangBatchTransition,
            "decode_update_prefix": SGLangDecodeUpdatePrefix,
            "admission_prefix": SGLangAdmissionPrefix,
            "admission_budget": SGLangAdmissionBudget,
            "prefill_statistics": SGLangPrefillStatistics,
            "request_publication": SGLangRequestPublication,
            "last_locations": SGLangLastLocations,
            "last_location": SGLangLastLocation,
        }
        methods = {}
        for binding in regions["bindings"]:
            region_type = inline_regions.get(binding["semantic"])
            if region_type is not None:
                region = region_type(observer, regions, binding)
                methods.setdefault(binding["hook"], []).append(region)

        for hook, method_regions in methods.items():
            method = SGLangInlineMethod(method_regions)
            HookRegistry.register(hook, method.invoke, HookType.AROUND)

    if cpu_sampling:
        scheduler = "sglang.srt.managers.scheduler.Scheduler"
        batch = "sglang.srt.managers.schedule_batch.ScheduleBatch"
        HookRegistry.register(f"{scheduler}.get_next_batch_to_run", observer.schedule, HookType.AROUND)
        HookRegistry.register(f"{batch}.prepare_for_extend", observer.prepare_prefill, HookType.AROUND)
        HookRegistry.register(f"{batch}.prepare_for_decode", observer.prepare_decode, HookType.AROUND)
        HookRegistry.register(f"{scheduler}.run_batch", observer.execute, HookType.AROUND)
        HookRegistry.register(f"{scheduler}.process_batch_result", observer.publish, HookType.AROUND)
    else:
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
        for hook, bindings in hooks.items():
            wrapper = wrappers[bindings[0]["wrapper"]](observer, definitions, bindings)
            HookRegistry.register(hook, wrapper.invoke, HookType.AROUND)

    if not cpu_sampling:
        regions = prepared["native_regions"]
        region_wrappers = {
            "batch_output": SGLangOutputWrapper,
            "input_resolution": SGLangInputResolutionWrapper,
        }
        for binding in regions["bindings"]:
            if binding["semantic"] in region_wrappers:
                wrapper = SGLangRegionHook(observer, regions, binding, region_wrappers[binding["semantic"]])
                HookRegistry.register(binding["hook"], wrapper.invoke, HookType.AROUND)
            elif binding["semantic"] in {"batch_filter", "batch_merge"}:
                wrapper = SGLangBatchWrapper(observer, regions, binding)
                HookRegistry.register(binding["hook"], wrapper.invoke, HookType.AROUND)

        HookRegistry.register(
            "sglang.srt.managers.tp_worker.TpModelWorker.forward_batch_generation", observer.forward, HookType.AROUND
        )
        HookRegistry.register(
            "sglang.srt.managers.scheduler_components.profiler_manager.SchedulerProfilerManager._start_profile",
            observer.start_profile,
            HookType.AROUND,
        )
        HookRegistry.register(
            observer.profile.configuration_hook,
            observer.profile.configure,
            HookType.AROUND,
        )
        HookRegistry.register(
            "sglang.srt.managers.scheduler_components.profiler_manager.SchedulerProfilerManager._stop_profile",
            observer.stop_profile,
            HookType.AROUND,
        )
