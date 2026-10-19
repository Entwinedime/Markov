"""Observe native batch provenance for independent host measurements."""

from __future__ import annotations

import atexit
import json
import os
import time
from collections.abc import Callable, Iterator
from contextlib import AbstractContextManager, contextmanager, nullcontext
from pathlib import Path
from typing import Any

from markov_collection.adapters.frameworks.sglang.batch_observer import SGLangBatchObserver
from markov_collection.adapters.runtimes.ascend.environment import RuntimeEnvironment
from markov_collection.adapters.runtimes.ascend.transfer_sampling import TransferSampling
from markov_collection.artifacts.json_file import write_json
from markov_collection.instrumentation.cpu_sampling import CpuSampling


class SGLangHostSampling(SGLangBatchObserver):
    def __init__(self, directory: Path, request_ids: set[str], control_ids: set[str], *, cpu: bool) -> None:
        super().__init__(request_ids)
        self.directory = directory
        self._sampling = TransferSampling(directory.parent / "cpu_workers")
        self._cpu = CpuSampling(directory.parent / "cpu_activities") if cpu else None
        self._control_ids = control_ids
        self._sequence = 0
        self._rank: int | None = None
        self._records: list[dict[str, Any]] = []
        self._scopes: list[dict[str, Any]] = []
        atexit.register(self.close)

    def _observer(self) -> AbstractContextManager[None]:
        return self._cpu.observer() if self._cpu is not None else nullcontext()

    def _control(self, requests: list[Any]) -> bool:
        return any(request.rid in self._control_ids and not request.finished() for request in requests)

    @contextmanager
    def _scope(self, step: str, metadata: dict[str, Any] | None, *, control: bool = False) -> Iterator[dict[str, Any]]:
        with self._observer():
            self._sequence += 1
            record = {
                "scope_id": self._sequence,
                "pid": os.getpid(),
                "rank": self._rank,
                "step": step,
                "metadata": metadata,
                "completed": False,
                "control": control,
                "control_interval": [[self._sequence, 0], [self._sequence, 1]],
            }
            self._scopes.append(record)
        try:
            with self._sampling.scope(0 if control else self._sequence):
                scope = (
                    self._cpu.scope(self._sequence, step) if self._cpu is not None and not control else nullcontext()
                )
                with scope:
                    with self._observer():
                        record["entered_ns"] = time.perf_counter_ns()
                    yield record
                    with self._observer():
                        record["completed"] = True
        finally:
            with self._observer():
                self._scopes.pop()
                if self._cpu is not None or record["metadata"] is not None:
                    self._records.append(record)

    def schedule(self, original: Callable[..., Any], scheduler: Any, *args: Any, **kwargs: Any) -> Any:
        with self._observer():
            visible = scheduler.waiting_queue + scheduler.running_batch.reqs
            if scheduler.last_batch is not None:
                visible = visible + scheduler.last_batch.reqs
            tracked = self._tracked(visible)
            control = self._control(visible)
        if not tracked:
            return original(scheduler, *args, **kwargs)

        self._rank = scheduler.ps.tp_rank
        with self._scope("schedule", None, control=control) as record:
            with self._observer():
                previous = scheduler.last_batch
                pending = previous is not None and previous.forward_mode.is_extend()
                record["scheduling"] = {
                    "transition": {
                        "running_requests": len(scheduler.running_batch.reqs),
                        "pending_prefill_requests": len(previous.reqs) if pending else None,
                        "admission_blocked": bool(scheduler.running_batch.batch_is_full),
                    },
                    "waiting_requests": len(scheduler.waiting_queue),
                    "filters": [],
                    "merge": None,
                }
            batch = original(scheduler, *args, **kwargs)
            with self._observer():
                record["native_return_ns"] = time.perf_counter_ns()
                if batch is not None:
                    phase = "prefill" if batch.forward_mode.is_extend() else "decode"
                    record["metadata"] = self._batch(batch, phase)
                else:
                    # An empty decision handles no model batch. Associate it
                    # with the live sampling cohort without inventing members.
                    record["metadata"] = {
                        "phase": "idle",
                        "requests": [],
                        "scope_request_ids": [
                            req.rid for req in visible if req.rid in self.request_ids and not req.finished()
                        ],
                    }

        return batch

    def filter(self, original: Callable[..., Any], batch: Any, *args: Any, **kwargs: Any) -> Any:
        with self._observer():
            owner = self._scopes[-1] if self._scopes and self._scopes[-1]["step"] == "schedule" else None
            before = len(batch.reqs)
        result = original(batch, *args, **kwargs)
        if owner is not None:
            with self._observer():
                owner["scheduling"]["filters"].append([before, len(batch.reqs)])
        return result

    def merge(self, original: Callable[..., Any], batch: Any, other: Any, *args: Any, **kwargs: Any) -> Any:
        with self._observer():
            if self._scopes and self._scopes[-1]["step"] == "schedule":
                self._scopes[-1]["scheduling"]["merge"] = {
                    "left_count": len(batch.reqs),
                    "right_count": len(other.reqs),
                }
        return original(batch, other, *args, **kwargs)

    def prepare_prefill(self, original: Callable[..., Any], batch: Any, *args: Any, **kwargs: Any) -> Any:
        return self._prepare(original, batch, "prefill", *args, **kwargs)

    def prepare_decode(self, original: Callable[..., Any], batch: Any, *args: Any, **kwargs: Any) -> Any:
        return self._prepare(original, batch, "decode", *args, **kwargs)

    def _prepare(self, original: Callable[..., Any], batch: Any, phase: str, *args: Any, **kwargs: Any) -> Any:
        with self._observer():
            tracked = self._tracked(batch.reqs)
        if not tracked:
            return original(batch, *args, **kwargs)

        with self._observer():
            self._batch_sequence += 1
            self._batch_ids[id(batch)] = self._batch_sequence
            metadata = self._batch(batch, phase)
            control = self._control(batch.reqs)
        with self._scope("prepare", metadata, control=control) as record:
            with self._observer():
                # SGLang selects the batch before entering Prepare; Prepare
                # owns the scheduler suffix through the parent's native return.
                if len(self._scopes) > 1 and self._scopes[-2]["step"] == "schedule":
                    parent = self._scopes[-2]
                    record["control_interval"][1] = [parent["scope_id"], 1]
                    parent["control_interval"][1] = [record["scope_id"], 0]
            result = original(batch, *args, **kwargs)
            with self._observer():
                record["native_return_ns"] = time.perf_counter_ns()
            return result

    def execute(self, original: Callable[..., Any], scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(scheduler, batch, *args, **kwargs)

        phase = "prefill" if batch.forward_mode.is_extend() else "decode"
        with self._scope(phase, self._batch(batch, phase), control=self._control(batch.reqs)) as record:
            result = original(scheduler, batch, *args, **kwargs)
            with self._observer():
                record["native_return_ns"] = time.perf_counter_ns()
            return result

    def publish(self, original: Callable[..., Any], scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(scheduler, batch, *args, **kwargs)

        phase = "prefill" if batch.forward_mode.is_extend() else "decode"
        metadata = self._batch(batch, phase)
        with self._scope("publish", metadata, control=self._control(batch.reqs)) as record:
            result = original(scheduler, batch, *args, **kwargs)
            with self._observer():
                record["native_return_ns"] = time.perf_counter_ns()
                finished = [req.rid for req in batch.reqs if req.finished()]
                metadata["finished_request_ids"] = finished

        self._finished_ids.update(finished)
        # Native workers can exit without Python's atexit callbacks.
        if self._finished_ids == self.request_ids:
            self.close()

        return result

    def close(self) -> None:
        self._sampling.flush()
        if self._cpu is not None:
            self._cpu.flush()
        if not self._records:
            return

        self.directory.mkdir(parents=True, exist_ok=True)
        write_json(
            self.directory / f"rank-{self._rank}-environment.json",
            {
                "rank": self._rank,
                "pid": os.getpid(),
                "runtime": RuntimeEnvironment().describe(),
            },
        )
        with (self.directory / f"rank-{self._rank}-pid-{os.getpid()}.jsonl").open("a", encoding="utf-8") as stream:
            for record in self._records:
                stream.write(json.dumps(record, separators=(",", ":")) + "\n")
        self._records.clear()


def install(prepared: dict[str, Any]) -> None:
    from sglang.srt.plugins.hook_registry import HookRegistry, HookType

    cpu = prepared["measurement_kind"] == "cpu"
    if cpu:
        from markov_collection.adapters.runtimes.pytorch.cpu_sampling import observe_operators

        observe_operators()

    observer = SGLangHostSampling(
        Path(os.environ["MARKOV_TRANSFER_SCOPES"]),
        set(prepared["measurement_ids"]) | set(prepared["warmup_ids"]) | set(prepared["control_ids"]),
        set(prepared["control_ids"]),
        cpu=cpu,
    )
    scheduler = "sglang.srt.managers.scheduler.Scheduler"
    batch = "sglang.srt.managers.schedule_batch.ScheduleBatch"
    HookRegistry.register(f"{scheduler}.get_next_batch_to_run", observer.schedule, HookType.AROUND)
    HookRegistry.register(f"{batch}.prepare_for_extend", observer.prepare_prefill, HookType.AROUND)
    HookRegistry.register(f"{batch}.prepare_for_decode", observer.prepare_decode, HookType.AROUND)
    HookRegistry.register(f"{scheduler}.run_batch", observer.execute, HookType.AROUND)
    HookRegistry.register(f"{scheduler}.process_batch_result", observer.publish, HookType.AROUND)
    if cpu:
        HookRegistry.register(f"{batch}.filter_batch", observer.filter, HookType.AROUND)
        HookRegistry.register(f"{batch}.merge_batch", observer.merge, HookType.AROUND)
