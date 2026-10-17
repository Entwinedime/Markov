"""Read native batch facts shared by structure and performance observations."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.torch_npu.profiling import AscendProfileWindow
from markov_collection.adapters.runtimes.pytorch.profiling import TorchProfilerMarkers
from markov_collection.instrumentation.recording import Recorder


class SGLangBatchObserver:
    def __init__(self, request_ids: set[str]) -> None:
        self.request_ids = request_ids
        self._batch_sequence = 0
        self._batch_ids: dict[int, int] = {}
        self._finished_ids: set[str] = set()

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


class SGLangProfileObserver(SGLangBatchObserver):
    def __init__(self, directory: Path, request_ids: set[str]) -> None:
        super().__init__(request_ids)
        self.directory = directory
        self.markers = TorchProfilerMarkers()
        self.recorder: Recorder | None = None
        self.profile = AscendProfileWindow(directory.parent / "workspace_states")

    def _recorder(self, rank: int) -> Recorder:
        if self.recorder is None:
            self.recorder = Recorder(self.directory, rank, self.markers)

        return self.recorder

    def start_profile(self, original: Callable[..., Any], manager: Any, *args: Any, **kwargs: Any) -> Any:
        self.profile.begin(manager.ps.tp_rank)
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
