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
    def __init__(self, directory: Path, request_ids: set[str]) -> None:
        super().__init__(request_ids)
        self.directory = directory
        self._ports: list[dict[str, Any]] = []

    def execute(self, original: Callable[..., Any], scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(scheduler, batch, *args, **kwargs)

        self._batch_sequence += 1
        self._batch_ids[id(batch)] = self._batch_sequence
        phase = "prefill" if batch.forward_mode.is_extend() else "decode"
        metadata = self._batch(batch, phase)

        result = original(scheduler, batch, *args, **kwargs)
        at_ns = time.perf_counter_ns()
        self._ports.append({"port": "submitted", "at_ns": at_ns, "metadata": metadata, "result": {}})

        return result

    def publish(self, original: Callable[..., Any], scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(scheduler, batch, *args, **kwargs)

        phase = "prefill" if batch.forward_mode.is_extend() else "decode"
        metadata = self._batch(batch, phase)

        # This is the original native method's ScopeReturn. The profiled path's
        # marker.end also encloses observer bookkeeping and is a different instant.
        result = original(scheduler, batch, *args, **kwargs)
        at_ns = time.perf_counter_ns()
        requests = [
            {"id": req.rid, "generated_tokens": len(req.output_ids), "finished": req.finished()} for req in batch.reqs
        ]
        self._ports.append(
            {"port": "published", "at_ns": at_ns, "metadata": metadata, "result": {"requests": requests}}
        )

        self._finished_ids.update(
            request["id"] for request in requests if request["finished"] and request["id"] in self.request_ids
        )
        if self._finished_ids == self.request_ids:
            # All workers and the request driver share this host's MONOTONIC
            # clock. This does not establish a clock relation across machines.
            rank = scheduler.ps.tp_rank
            write_json(
                self.directory / f"rank-{rank}-ports.json",
                {
                    "rank": rank,
                    "pid": os.getpid(),
                    "clock": {
                        "name": "perf_counter_ns",
                        "implementation": time.get_clock_info("perf_counter").implementation,
                    },
                    "ports": self._ports,
                },
            )

        return result


def install(prepared: dict[str, Any]) -> None:
    from sglang.srt.plugins.hook_registry import HookRegistry, HookType

    observer = ServingPortObserver(
        Path(os.environ["MARKOV_OBSERVATIONS"]), {request["id"] for request in prepared["requests"]}
    )
    scheduler = "sglang.srt.managers.scheduler.Scheduler"
    HookRegistry.register(f"{scheduler}.run_batch", observer.execute, HookType.AROUND)
    HookRegistry.register(f"{scheduler}.process_batch_result", observer.publish, HookType.AROUND)
