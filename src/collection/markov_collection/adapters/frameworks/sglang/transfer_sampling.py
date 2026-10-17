"""Observe actual batch provenance for synchronous host-transfer measurements."""

from __future__ import annotations

import atexit
import json
import os
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from pathlib import Path
from typing import Any

from markov_collection.adapters.frameworks.sglang.batch_observer import SGLangBatchObserver
from markov_collection.adapters.runtimes.ascend.environment import RuntimeEnvironment
from markov_collection.adapters.runtimes.ascend.transfer_sampling import TransferSampling
from markov_collection.artifacts.json_file import write_json


class TransferSamplingObserver(SGLangBatchObserver):
    def __init__(self, directory: Path, request_ids: set[str]) -> None:
        super().__init__(request_ids)
        self.directory = directory
        self._sampling = TransferSampling(directory.parent / "cpu_workers")
        self._sequence = 0
        self._rank: int | None = None
        self._records: list[dict[str, Any]] = []
        atexit.register(self.close)

    @contextmanager
    def _scope(self, step: str, metadata: dict[str, Any] | None) -> Iterator[dict[str, Any]]:
        self._sequence += 1
        record = {
            "scope_id": self._sequence,
            "pid": os.getpid(),
            "rank": self._rank,
            "step": step,
            "metadata": metadata,
            "completed": False,
        }
        try:
            with self._sampling.scope(self._sequence):
                yield record
                record["completed"] = True
        finally:
            if record["metadata"] is not None:
                self._records.append(record)

    def schedule(self, original: Callable[..., Any], scheduler: Any, *args: Any, **kwargs: Any) -> Any:
        visible = scheduler.waiting_queue + scheduler.running_batch.reqs
        if scheduler.last_batch is not None:
            visible = visible + scheduler.last_batch.reqs
        if not self._tracked(visible):
            return original(scheduler, *args, **kwargs)

        self._rank = scheduler.ps.tp_rank
        with self._scope("schedule", None) as record:
            batch = original(scheduler, *args, **kwargs)
            if batch is not None:
                phase = "prefill" if batch.forward_mode.is_extend() else "decode"
                record["metadata"] = self._batch(batch, phase)

        return batch

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
        with self._scope("prepare", metadata):
            return original(batch, *args, **kwargs)

    def execute(self, original: Callable[..., Any], scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(scheduler, batch, *args, **kwargs)

        phase = "prefill" if batch.forward_mode.is_extend() else "decode"
        with self._scope(phase, self._batch(batch, phase)):
            return original(scheduler, batch, *args, **kwargs)

    def publish(self, original: Callable[..., Any], scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(scheduler, batch, *args, **kwargs)

        phase = "prefill" if batch.forward_mode.is_extend() else "decode"
        metadata = self._batch(batch, phase)
        with self._scope("publish", metadata):
            result = original(scheduler, batch, *args, **kwargs)
            finished = [req.rid for req in batch.reqs if req.finished()]
            metadata["finished_request_ids"] = finished

        self._finished_ids.update(finished)
        # Native workers can exit without Python's atexit callbacks.
        if self._finished_ids == self.request_ids:
            self.close()

        return result

    def close(self) -> None:
        self._sampling.flush()
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

    observer = TransferSamplingObserver(
        Path(os.environ["MARKOV_TRANSFER_SCOPES"]), set(prepared["measurement_ids"]) | set(prepared["warmup_ids"])
    )
    scheduler = "sglang.srt.managers.scheduler.Scheduler"
    batch = "sglang.srt.managers.schedule_batch.ScheduleBatch"
    HookRegistry.register(f"{scheduler}.get_next_batch_to_run", observer.schedule, HookType.AROUND)
    HookRegistry.register(f"{batch}.prepare_for_extend", observer.prepare_prefill, HookType.AROUND)
    HookRegistry.register(f"{batch}.prepare_for_decode", observer.prepare_decode, HookType.AROUND)
    HookRegistry.register(f"{scheduler}.run_batch", observer.execute, HookType.AROUND)
    HookRegistry.register(f"{scheduler}.process_batch_result", observer.publish, HookType.AROUND)
