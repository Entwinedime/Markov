"""Observe native serving return ports without profiling or semantic instrumentation."""

from __future__ import annotations

import os
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from markov_collection.adapters.frameworks.sglang.batch_observer import SGLangBatchObserver
from markov_collection.artifacts.json_file import write_json


class ServingPortObserver(SGLangBatchObserver):
    def __init__(self, directory: Path, request_ids: set[str], *, source_rank: int | None = None) -> None:
        super().__init__(request_ids, source_rank=source_rank)
        self.directory = directory
        self._ports: list[dict[str, Any]] = []
        self._outputs: list[dict[str, Any]] = []
        self._arrivals: list[dict[str, Any]] = []

    @staticmethod
    def _native_call(original: Callable[..., Any], *args: Any, **kwargs: Any) -> tuple[Any, dict[str, Any]]:
        # Two coarse clocks diagnose control-thread work without installing
        # cost callbacks. These aggregates include CPU used by waits/transfers;
        # they are validation observations, never training labels.
        started_ns = time.perf_counter_ns()
        cpu_started_ns = time.thread_time_ns()
        result = original(*args, **kwargs)
        at_ns = time.perf_counter_ns()
        thread_cpu_ns = time.thread_time_ns() - cpu_started_ns

        return result, {"at_ns": at_ns, "native_call": {"started_ns": started_ns, "thread_cpu_ns": thread_cpu_ns}}

    def arrive(self, original: Callable[..., Any], scheduler: Any, request: Any, *args: Any, **kwargs: Any) -> Any:
        if request.rid not in self.request_ids:
            return original(scheduler, request, *args, **kwargs)

        result = original(scheduler, request, *args, **kwargs)
        at_ns = time.perf_counter_ns()
        # The native request is now ready for scheduling. Earlier ingress work
        # remains in the separately reported driver interval.
        self._arrivals.append({"request_id": request.rid, "at_ns": at_ns})
        return result

    def execute(self, original: Callable[..., Any], scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(scheduler, batch, *args, **kwargs)

        self._batch_sequence += 1
        self._batch_ids[id(batch)] = self._batch_sequence
        phase = "prefill" if batch.forward_mode.is_extend() else "decode"
        metadata = self._batch(batch, phase)

        result, timing = self._native_call(original, scheduler, batch, *args, **kwargs)
        self._ports.append({"port": "submitted", **timing, "metadata": metadata, "result": {}})

        return result

    def publish(self, original: Callable[..., Any], scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(scheduler, batch, *args, **kwargs)

        phase = "prefill" if batch.forward_mode.is_extend() else "decode"
        metadata = self._batch(batch, phase)

        # This is the original native method's ScopeReturn. The profiled path's
        # marker.end also encloses observer bookkeeping and is a different instant.
        result, timing = self._native_call(original, scheduler, batch, *args, **kwargs)
        requests = [
            {"id": req.rid, "generated_tokens": len(req.output_ids), "finished": req.finished()} for req in batch.reqs
        ]
        self._ports.append(
            {"port": "result_processed", **timing, "metadata": metadata, "result": {"requests": requests}}
        )

        self._finished_ids.update(
            request["id"] for request in requests if request["finished"] and request["id"] in self.request_ids
        )
        if self._finished_ids == self.request_ids:
            # All workers and the request driver share this host's MONOTONIC
            # clock. This does not establish a clock relation across machines.
            rank = self.source_rank(scheduler.ps.tp_rank)
            write_json(
                self.directory / f"rank-{rank}-ports.json",
                {
                    "rank": rank,
                    "publishes_outputs": scheduler.send_to_detokenizer.socket is not None,
                    "pid": os.getpid(),
                    "clock": {
                        "name": "perf_counter_ns",
                        "implementation": time.get_clock_info("perf_counter").implementation,
                    },
                    "arrivals": self._arrivals,
                    "ports": self._ports,
                    "outputs": self._outputs,
                },
            )

        return result

    def output(self, original: Callable[..., Any], streamer: Any, reqs: list[Any], *args: Any, **kwargs: Any) -> Any:
        if streamer.send_to_detokenizer.socket is None:
            return original(streamer, reqs, *args, **kwargs)

        requests = [(req, req.send_token_offset) for req in reqs if req.rid in self.request_ids]
        if not requests:
            return original(streamer, reqs, *args, **kwargs)

        result = original(streamer, reqs, *args, **kwargs)
        at_ns = time.perf_counter_ns()
        # The native sender has returned. Offsets identify the requests whose
        # token payload was handed downstream; an empty batch is not output.
        for request, previous in requests:
            if request.send_token_offset > previous:
                self._outputs.append(
                    {
                        "request_id": request.rid,
                        "at_ns": at_ns,
                        "tokens": request.send_token_offset - previous,
                        "cumulative_tokens": request.send_token_offset,
                        "finished": request.finished(),
                    }
                )
        return result


def install(prepared: dict[str, Any]) -> None:
    from sglang.srt.plugins.hook_registry import HookRegistry, HookType

    observer = ServingPortObserver(
        Path(os.environ["MARKOV_OBSERVATIONS"]),
        {request["id"] for request in prepared["requests"]},
        source_rank=prepared.get("shard_execution", {}).get("rank"),
    )
    scheduler = "sglang.srt.managers.scheduler.Scheduler"
    HookRegistry.register(f"{scheduler}.handle_generate_request", observer.arrive, HookType.AROUND)
    HookRegistry.register(f"{scheduler}.run_batch", observer.execute, HookType.AROUND)
    HookRegistry.register(f"{scheduler}.process_batch_result", observer.publish, HookType.AROUND)
    HookRegistry.register(
        "sglang.srt.managers.scheduler_components.output_streamer.SchedulerOutputStreamer.stream_output",
        observer.output,
        HookType.AROUND,
    )
