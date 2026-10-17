"""Associate device work with actual batches without building semantic templates."""

from __future__ import annotations

import os
from collections.abc import Callable
from pathlib import Path
from typing import Any

from markov_collection.adapters.frameworks.sglang.batch_observer import SGLangProfileObserver


class DeviceSamplingObserver(SGLangProfileObserver):
    def filter(self, original: Callable[..., Any], batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(batch, *args, **kwargs)

        with self.markers.observer():
            positions = {id(request): index for index, request in enumerate(batch.reqs)}
            # Filtering follows publication. These are current native member
            # facts, not the input/context lengths of another model execution.
            metadata = {
                "requests": [
                    {
                        "id": request.rid,
                        "prompt_tokens": len(request.origin_input_ids),
                        "generated_tokens": len(request.output_ids),
                    }
                    for request in batch.reqs
                ]
            }
            observation = self.recorder.scope("filter", metadata)

        with observation:
            result = original(batch, *args, **kwargs)
            with self.markers.observer():
                # Native filtering owns membership and order. Observe its result
                # without copying device indices or repeating its selection rule.
                metadata["batch_filter"] = {
                    "input_count": len(positions),
                    "retained_indices": [positions[id(request)] for request in batch.reqs],
                }

        return result

    def _native_members(self, batch: Any, metadata: dict[str, Any]) -> None:
        # Slots are native reservations, not positions in the request list.
        # Read them before publication can release a finished request's slot.
        slots = {request.rid: request.req_pool_idx for request in batch.reqs}
        metadata["request_pool_shape"] = list(batch.req_to_token_pool.req_to_token.shape)
        for request in metadata["requests"]:
            request["request_pool_slot"] = slots[request["id"]]

        if metadata["phase"] == "prefill":
            # Native preparation constructs these host lists for the actual
            # position and table-write calls. No tensor contents are inspected.
            lengths = {
                request.rid: (prefix, extend)
                for request, prefix, extend in zip(batch.reqs, batch.prefix_lens, batch.extend_lens, strict=True)
            }
            for request in metadata["requests"]:
                request["context_tokens"], request["input_tokens"] = lengths[request["id"]]

    def prepare_prefill(self, original: Callable[..., Any], batch: Any, *args: Any, **kwargs: Any) -> Any:
        return self._prepare(original, batch, "prefill", *args, **kwargs)

    def prepare_decode(self, original: Callable[..., Any], batch: Any, *args: Any, **kwargs: Any) -> Any:
        return self._prepare(original, batch, "decode", *args, **kwargs)

    def _prepare(self, original: Callable[..., Any], batch: Any, phase: str, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(batch, *args, **kwargs)

        self._batch_sequence += 1
        self._batch_ids[id(batch)] = self._batch_sequence
        metadata = self._batch(batch, phase)
        with self.recorder.scope("prepare", metadata):
            result = original(batch, *args, **kwargs)
            with self.markers.observer():
                self._native_members(batch, metadata)

        return result

    def execute(self, original: Callable[..., Any], scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(scheduler, batch, *args, **kwargs)

        with self.markers.observer():
            phase = "prefill" if batch.forward_mode.is_extend() else "decode"
            metadata = self._batch(batch, phase)
            self._native_members(batch, metadata)
            observation = self._recorder(scheduler.ps.tp_rank).scope(phase, metadata)

        with observation:
            return original(scheduler, batch, *args, **kwargs)

    def publish(self, original: Callable[..., Any], scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(scheduler, batch, *args, **kwargs)

        with self.markers.observer():
            phase = "prefill" if batch.forward_mode.is_extend() else "decode"
            metadata = self._batch(batch, phase)
            # Completed-point coverage must retain the actual reservations
            # before native publication releases any finished request's slot.
            self._native_members(batch, metadata)
            observation = self.recorder.scope("publish", metadata)

        with observation:
            result = original(scheduler, batch, *args, **kwargs)
            finished = [req.rid for req in batch.reqs if req.finished()]
            metadata["finished_request_ids"] = finished

        self._finished_ids.update(finished)
        # The outer sampler acknowledges the next scheduler iteration before
        # shutdown; client replies alone do not prove observations are persisted.
        if self._finished_ids == self.request_ids:
            self.recorder.flush_native(close_window=True)

        return result


def install(prepared: dict[str, Any]) -> None:
    from sglang.srt.plugins.hook_registry import HookRegistry, HookType

    observer = DeviceSamplingObserver(
        Path(os.environ["MARKOV_OBSERVATIONS"]), set(prepared["measurement_ids"]) | set(prepared["warmup_ids"])
    )
    batch = "sglang.srt.managers.schedule_batch.ScheduleBatch"
    scheduler = "sglang.srt.managers.scheduler.Scheduler"
    HookRegistry.register(f"{batch}.filter_batch", observer.filter, HookType.AROUND)
    HookRegistry.register(f"{batch}.prepare_for_extend", observer.prepare_prefill, HookType.AROUND)
    HookRegistry.register(f"{batch}.prepare_for_decode", observer.prepare_decode, HookType.AROUND)
    HookRegistry.register(f"{scheduler}.run_batch", observer.execute, HookType.AROUND)
    HookRegistry.register(f"{scheduler}.process_batch_result", observer.publish, HookType.AROUND)
    observer.install_profile_hooks()
