"""Read native batch facts shared by structure and performance observations."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.torch_npu.profiling import AscendProfileWindow
from markov_collection.adapters.runtimes.pytorch.profiling import TorchProfilerMarkers
from markov_collection.instrumentation.recording import Recorder


class SGLangBatchObserver:
    def __init__(self, request_ids: set[str], *, source_rank: int | None = None) -> None:
        self.request_ids = request_ids
        self._source_rank = source_rank
        self._batch_sequence = 0
        self._batch_ids: dict[int, int] = {}
        self._finished_ids: set[str] = set()

    def source_rank(self, native_rank: int) -> int:
        # One physical worker may observe a fixed logical tensor-parallel role.
        # Its process and device identities remain native provenance.
        return native_rank if self._source_rank is None else self._source_rank

    def _tracked(self, reqs: list[Any]) -> bool:
        return any(req.rid in self.request_ids and not req.finished() for req in reqs)

    @staticmethod
    def _input_requests(messages: list[Any]) -> tuple[list[dict[str, Any]], bool]:
        from sglang.srt.managers.io_struct import BatchTokenizedGenerateReqInput, TokenizedGenerateReqInput

        requests = []
        supported = True
        for message in messages:
            if isinstance(message, BatchTokenizedGenerateReqInput):
                requests.extend(message)
            elif isinstance(message, TokenizedGenerateReqInput):
                requests.append(message)
            else:
                supported = False
        inputs = []
        for request in requests:
            params = request.sampling_params
            if request.input_ids is None or params.max_new_tokens is None:
                supported = False
                continue
            supported = supported and (
                request.session_params is None
                and request.mm_inputs is None
                and request.input_embeds is None
                and request.positional_embed_overrides is None
                and request.lora_id is None
                and request.custom_logit_processor is None
                and not request.return_logprob
                and not request.return_hidden_states
                and not request.return_routed_experts
                and not request.return_indexer_topk
                and params.json_schema is None
                and params.regex is None
                and params.ebnf is None
                and params.structural_tag is None
            )
            inputs.append(
                {
                    "id": request.rid,
                    "prompt_tokens": len(request.input_ids),
                    "output_tokens": params.max_new_tokens,
                }
            )
        return inputs, supported

    @staticmethod
    def _inputs_accepted(scheduler: Any, inputs: list[dict[str, Any]]) -> bool:
        waiting = {request.rid: request for request in scheduler.waiting_queue}
        return all(
            item["id"] in waiting
            and not waiting[item["id"]].finished()
            and len(waiting[item["id"]].origin_input_ids) == item["prompt_tokens"]
            and waiting[item["id"]].sampling_params.max_new_tokens == item["output_tokens"]
            for item in inputs
        )

    @staticmethod
    def _visible_requests(scheduler: Any) -> list[Any]:
        visible = scheduler.waiting_queue + scheduler.running_batch.reqs
        if scheduler.last_batch is not None:
            visible = visible + scheduler.last_batch.reqs
        return visible

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


class SGLangProfileObserver(SGLangBatchObserver):
    def __init__(self, directory: Path, request_ids: set[str], *, source_rank: int | None = None) -> None:
        super().__init__(request_ids, source_rank=source_rank)
        self.directory = directory
        self.markers = TorchProfilerMarkers()
        self.recorder: Recorder | None = None
        self.profile = AscendProfileWindow(directory.parent / "workspace_states")

    def _recorder(self, rank: int) -> Recorder:
        if self.recorder is None:
            self.recorder = Recorder(self.directory, self.source_rank(rank), self.markers)

        return self.recorder

    def start_profile(self, original: Callable[..., Any], manager: Any, *args: Any, **kwargs: Any) -> Any:
        self.profile.begin(self.source_rank(manager.ps.tp_rank))
        # Preserve the warm allocator state before profiler startup changes it.
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

    def install_profile_hooks(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        manager = "sglang.srt.managers.scheduler_components.profiler_manager.SchedulerProfilerManager"
        HookRegistry.register(f"{manager}._start_profile", self.start_profile, HookType.AROUND)
        HookRegistry.register(self.profile.configuration_hook, self.profile.configure, HookType.AROUND)
        HookRegistry.register(f"{manager}._stop_profile", self.stop_profile, HookType.AROUND)
