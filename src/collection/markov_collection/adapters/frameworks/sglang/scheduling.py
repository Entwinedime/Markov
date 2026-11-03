"""Observe one native scheduling round, including PP's split Prefill calls."""

from __future__ import annotations

import sys
from contextlib import ExitStack, contextmanager
from time import perf_counter_ns
from typing import Any


class ScheduleInvocation:
    def __init__(self, observation: Any, scheduler: Any) -> None:
        self.observation = observation
        self.scheduler = scheduler
        self.phase = observation.window.phase
        self.contexts = ExitStack()
        self.scope = None

    def __enter__(self) -> ScheduleInvocation:
        owner, scheduler = self.observation, self.scheduler
        if not owner.selected or owner.window.drained or self.phase not in ("source", "baseline"):
            self.phase = None
            return self

        # A logically idle worker may still need to finish its last asynchronous send.
        self.transfers = owner.pipeline.pending_work(scheduler) if owner.pipeline is not None else 0
        if owner.window.native_idle and not self.transfers:
            self.phase = None
            return self
        self.idle = scheduler.is_fully_idle()
        with ExitStack() as contexts:
            if self.phase == "source":
                owner.begin_batch(scheduler)
                self.scope = contexts.enter_context(owner.recording.scope("Schedule"))
                contexts.enter_context(owner.operators)
                self.scope.metadata["ports"] = {
                    "inputs": owner.operators.values.port_values(
                        {"free_pages": scheduler.token_to_kv_pool_allocator.free_pages}
                    ),
                    "outputs": {},
                }
            else:
                identity = len(owner.recording.batches)
                contexts.enter_context(owner.recording.markers.scope(f"markov.baseline/{identity}"))
            self.contexts = contexts.pop_all()
        return self

    def __exit__(self, kind: Any, error: Any, traceback: Any) -> None:
        self.contexts.__exit__(kind, error, traceback)

    def boundary(self, name: str) -> None:
        if self.scope is not None:

            # These are control handoff instants, not durations of the enclosing CPU scope.
            self.scope.metadata.setdefault("control_boundaries", {})[name] = perf_counter_ns()

    def complete(self, batch: Any) -> None:
        if self.phase is None:
            return
        owner, scheduler = self.observation, self.scheduler
        identity = owner.record_batch(scheduler, batch, self.phase)
        owner.recording.batches[identity]["scheduling_context"] = {
            "native_idle": self.idle,
            "idle_reported": owner.window.native_idle,
            "pending_transfers": self.transfers,
        }
        if self.scope is None:
            return
        self.scope.metadata["batch"] = identity
        if batch is None:
            self.scope.metadata["no_batch"] = True
        else:
            self.scope.metadata["ports"]["outputs"] = owner.operators.values.port_values(
                {
                    "request_slots": batch.req_pool_indices,
                    "sequence_lengths": batch.seq_lens,
                    "kv_locations": batch.out_cache_loc,
                    "free_pages": scheduler.token_to_kv_pool_allocator.free_pages,
                }
            )


class SGLangScheduling:
    def __init__(self, observation: Any) -> None:
        self.observation = observation
        self.current: ScheduleInvocation | None = None
        self.pending = None

    def inputs(self, original: Any, scheduler: Any, requests: Any) -> Any:
        if self.observation.pipeline is not None:

            # PD allocation precedes selection; establish the native lane before processing incoming requests.
            self.observation.batch_lane = next(
                index for index, batch in enumerate(scheduler.running_mbs) if batch is scheduler.running_batch
            )
        return original(scheduler, requests)

    @contextmanager
    def invocation(self, scheduler: Any):
        with ScheduleInvocation(self.observation, scheduler) as call:
            previous, self.current = self.current, call
            try:
                yield call
            finally:
                self.current = previous

    def schedule(self, original: Any, scheduler: Any) -> Any:
        with self.invocation(scheduler) as call:
            batch = original(scheduler)
            call.complete(batch)
            return batch

    def start_prefill(self, original: Any, scheduler: Any) -> Any:
        if self.pending is not None:
            raise RuntimeError("native PP Prefill started another scheduling round before coordination returned")

        # The native PP loop has no method enclosing chunk handling, admission and coordination together.
        self.pending = self.invocation(scheduler)
        self.pending.__enter__()
        return original(scheduler)

    def boundary(self, original: Any, adapter: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        call = self.current
        if call is not None and batch is not None:
            call.boundary("prepared_ns")
        result = original(adapter, batch, *args, **kwargs)
        if call is not None:
            call.boundary("scheduled_ns")
            if self.pending is not None:
                call.complete(result)
                self.close_pending(None, None, None)
        return result

    def close_pending(self, kind: Any, error: Any, traceback: Any) -> None:
        pending, self.pending = self.pending, None
        pending.__exit__(kind, error, traceback)

    def prefill_loop(self, original: Any, scheduler: Any, *args: Any, **kwargs: Any) -> Any:
        try:
            result = original(scheduler, *args, **kwargs)
        except BaseException:

            # Restore scope and dispatcher contexts if any of the three native calls fails.
            if self.pending is not None:
                self.close_pending(*sys.exc_info())
            raise
        if self.pending is not None:
            error = RuntimeError("native PP Prefill exited before completing its scheduling round")
            self.close_pending(type(error), error, None)
            raise error
        return result

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        owner = self.observation
        scheduler = "sglang.srt.managers.scheduler.Scheduler."
        targets = {
            scheduler + "process_input_requests": self.inputs,
            "sglang.srt.managers.scheduler_components.dp_attn.SchedulerDPAttnAdapter.maybe_prepare_mlp_sync_batch": (
                self.boundary
            )
        }
        if owner.disaggregation is None:
            targets[scheduler + "get_next_batch_to_run"] = self.schedule
        elif owner.pipeline is not None and owner.disaggregation.role == "prefill":
            targets[scheduler + "process_prefill_chunk"] = self.start_prefill
            targets[scheduler + "event_loop_pp_disagg_prefill"] = self.prefill_loop
        else:
            targets[scheduler + f"get_next_disagg_{owner.disaggregation.role}_batch_to_run"] = self.schedule
        for target, hook in targets.items():
            HookRegistry.register(target, hook, HookType.AROUND)
