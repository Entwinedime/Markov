"""Observe unprofiled native CPU phases; exclude nested preparation and its probes."""

from __future__ import annotations

import atexit
import ctypes
import json
import os
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from markov_collection.adapters.frameworks.sglang.observer import SGLangObserver
from markov_collection.adapters.runtimes.ascend.environment import RuntimeEnvironment
from markov_collection.artifacts.json_file import write_json


class RuntimeCpuCounters(ctypes.Structure):
    _fields_ = [
        (name, ctypes.c_int64)
        for name in (
            "wait_ns",
            "probe_ns",
            "wait_wall_ns",
            "last_sync_start_ns",
            "last_sync_end_ns",
            "last_sync_cpu_end_ns",
            "last_sync_probe_cpu_ns",
            "last_sync_result",
        )
    ]


@dataclass
class CpuPart:
    active_ns: int
    wait_ns: int
    probe_ns: int
    wait_wall_ns: int


@dataclass
class ScheduleSample:
    cpu_start_ns: int
    counters_start: RuntimeCpuCounters
    prefix: CpuPart | None = None
    preparation_wrapper_cpu_ns: int = 0
    preparation_wait_ns: int = 0
    preparation_probe_ns: int = 0
    preparation_wait_wall_ns: int = 0


class CpuSamplingObserver(SGLangObserver):
    def __init__(self, directory: Path, request_ids: set[str], warmup_ids: set[str], regions: dict[str, Any]) -> None:
        super().__init__(directory, request_ids | warmup_ids, regions)
        self._warmup_ids = warmup_ids
        self._schedule_sample: ScheduleSample | None = None
        self._records: list[dict[str, Any]] = []
        self._execution_start_ns = 0
        self._submission_ns = 0
        self._rank: int | None = None

        self._native = ctypes.CDLL(None)
        self._native.markov_native_begin_cpu_sampling.argtypes = []
        self._native.markov_native_begin_cpu_sampling.restype = None
        self._native.markov_native_read_cpu_counters.argtypes = [ctypes.POINTER(RuntimeCpuCounters)]
        self._native.markov_native_read_cpu_counters.restype = None
        self._native.markov_native_begin_cpu_sampling()

        atexit.register(self.close)

    def _counters(self) -> RuntimeCpuCounters:
        counters = RuntimeCpuCounters()
        self._native.markov_native_read_cpu_counters(ctypes.byref(counters))
        return counters

    def _write(
        self,
        step: str,
        metadata: dict[str, Any],
        start_ns: int,
        end_ns: int,
        parts: dict[str, CpuPart],
        ready_proxy: dict[str, Any] | None = None,
    ) -> None:
        nonwaiting = {name: value.active_ns - value.wait_ns - value.probe_ns for name, value in parts.items()}
        if any(value < 0 for value in nonwaiting.values()):
            raise ValueError("native CPU counters exceed the observed phase CPU time")

        self._records.append(
            {
                "step": step,
                "stage": "warmup" if metadata["requests"][0]["id"] in self._warmup_ids else "measurement",
                "rank": self._rank,
                "pid": os.getpid(),
                "tid": threading.get_native_id(),
                "start_ns": start_ns,
                "end_ns": end_ns,
                "metadata": metadata,
                "cpu_active_ns": {name: value.active_ns for name, value in parts.items()},
                "runtime_wait_cpu_ns": {name: value.wait_ns for name, value in parts.items()},
                "runtime_wait_wall_ns": {name: value.wait_wall_ns for name, value in parts.items()},
                "native_probe_cpu_ns": {name: value.probe_ns for name, value in parts.items()},
                "cpu_nonwaiting_active_ns": nonwaiting,
                "ready_proxy": ready_proxy,
            }
        )

    def close(self) -> None:
        if not self._records:
            return

        self.directory.mkdir(parents=True, exist_ok=True)
        write_json(
            self.directory / f"rank-{self._rank}-environment.json",
            {
                "rank": self._rank,
                "pid": os.getpid(),
                "cpu_affinity": sorted(os.sched_getaffinity(0)),
                "runtime": RuntimeEnvironment().describe(),
            },
        )
        with (self.directory / f"rank-{self._rank}-pid-{os.getpid()}.jsonl").open("a", encoding="utf-8") as stream:
            for record in self._records:
                stream.write(json.dumps(record, separators=(",", ":")) + "\n")

        self._records.clear()

    def schedule(self, original: Callable[..., Any], scheduler: Any, *args: Any, **kwargs: Any) -> Any:
        visible = scheduler.waiting_queue + scheduler.running_batch.reqs
        if scheduler.last_batch is not None:
            visible = visible + scheduler.last_batch.reqs
        if not self._tracked(visible):
            return original(scheduler, *args, **kwargs)

        self._rank = scheduler.ps.tp_rank
        state_features = {
            "waiting_request_count": len(scheduler.waiting_queue),
            "running_request_count": len(scheduler.running_batch.reqs),
            "last_batch_request_count": len(scheduler.last_batch.reqs) if scheduler.last_batch is not None else 0,
        }

        start_ns = time.perf_counter_ns()
        sample = ScheduleSample(0, self._counters())
        self._schedule_sample = sample
        sample.cpu_start_ns = time.thread_time_ns()
        try:
            batch = original(scheduler, *args, **kwargs)
        finally:
            cpu_end_ns = time.thread_time_ns()
            end_ns = time.perf_counter_ns()
            counters_end = self._counters()
            self._schedule_sample = None

        if sample.prefix is None:
            raise RuntimeError("CPU sampling observed a tracked schedule without native preparation")

        prefix = sample.prefix

        # Preparation is nested inside schedule; subtract it before recording the suffix.
        suffix = CpuPart(
            cpu_end_ns - sample.cpu_start_ns - prefix.active_ns - sample.preparation_wrapper_cpu_ns,
            counters_end.wait_ns - sample.counters_start.wait_ns - prefix.wait_ns - sample.preparation_wait_ns,
            counters_end.probe_ns - sample.counters_start.probe_ns - prefix.probe_ns - sample.preparation_probe_ns,
            counters_end.wait_wall_ns
            - sample.counters_start.wait_wall_ns
            - prefix.wait_wall_ns
            - sample.preparation_wait_wall_ns,
        )

        phase = "prefill" if batch.forward_mode.is_extend() else "decode"
        metadata = self._batch(batch, phase)
        metadata.update(state_features)

        self._write(
            "schedule",
            metadata,
            start_ns,
            end_ns,
            {"schedule_prefix": prefix, "prepare_suffix": suffix},
        )

        return batch

    def prepare_prefill(self, original: Callable[..., Any], batch: Any, *args: Any, **kwargs: Any) -> Any:
        return self._prepare(original, batch, "prefill", *args, **kwargs)

    def prepare_decode(self, original: Callable[..., Any], batch: Any, *args: Any, **kwargs: Any) -> Any:
        return self._prepare(original, batch, "decode", *args, **kwargs)

    def _prepare(self, original: Callable[..., Any], batch: Any, phase: str, *args: Any, **kwargs: Any) -> Any:
        wrapper_cpu_start_ns = time.thread_time_ns()
        wrapper_counters = self._counters()
        if not self._tracked(batch.reqs):
            return original(batch, *args, **kwargs)

        sample = self._schedule_sample
        sample.prefix = CpuPart(
            wrapper_cpu_start_ns - sample.cpu_start_ns,
            wrapper_counters.wait_ns - sample.counters_start.wait_ns,
            wrapper_counters.probe_ns - sample.counters_start.probe_ns,
            wrapper_counters.wait_wall_ns - sample.counters_start.wait_wall_ns,
        )

        self._batch_sequence += 1
        self._batch_ids[id(batch)] = self._batch_sequence
        metadata = self._batch(batch, phase)

        start_ns = time.perf_counter_ns()
        counters_start = self._counters()
        cpu_start_ns = time.thread_time_ns()
        result = original(batch, *args, **kwargs)
        cpu_end_ns = time.thread_time_ns()
        end_ns = time.perf_counter_ns()
        counters_end = self._counters()

        sample.preparation_wait_ns = counters_end.wait_ns - wrapper_counters.wait_ns
        sample.preparation_probe_ns = counters_end.probe_ns - wrapper_counters.probe_ns
        sample.preparation_wait_wall_ns = counters_end.wait_wall_ns - wrapper_counters.wait_wall_ns

        self._write(
            "prepare",
            metadata,
            start_ns,
            end_ns,
            {
                "prepare_body": CpuPart(
                    cpu_end_ns - cpu_start_ns,
                    counters_end.wait_ns - counters_start.wait_ns,
                    counters_end.probe_ns - counters_start.probe_ns,
                    counters_end.wait_wall_ns - counters_start.wait_wall_ns,
                )
            },
        )

        sample.preparation_wrapper_cpu_ns = time.thread_time_ns() - wrapper_cpu_start_ns

        return result

    def execute(self, original: Callable[..., Any], scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(scheduler, batch, *args, **kwargs)

        phase = "prefill" if batch.forward_mode.is_extend() else "decode"
        metadata = self._batch(batch, phase)

        start_ns = time.perf_counter_ns()
        counters_start = self._counters()
        cpu_start_ns = time.thread_time_ns()
        result = original(scheduler, batch, *args, **kwargs)
        cpu_end_ns = time.thread_time_ns()
        end_ns = time.perf_counter_ns()
        counters_end = self._counters()

        self._execution_start_ns = start_ns
        self._submission_ns = end_ns

        self._write(
            phase,
            metadata,
            start_ns,
            end_ns,
            {
                "execute": CpuPart(
                    cpu_end_ns - cpu_start_ns,
                    counters_end.wait_ns - counters_start.wait_ns,
                    counters_end.probe_ns - counters_start.probe_ns,
                    counters_end.wait_wall_ns - counters_start.wait_wall_ns,
                )
            },
        )

        return result

    def publish(self, original: Callable[..., Any], scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not self._tracked(batch.reqs):
            return original(scheduler, batch, *args, **kwargs)

        phase = "prefill" if batch.forward_mode.is_extend() else "decode"
        metadata = self._batch(batch, phase)

        start_ns = time.perf_counter_ns()
        counters_start = self._counters()
        cpu_start_ns = time.thread_time_ns()
        result = original(scheduler, batch, *args, **kwargs)
        cpu_end_ns = time.thread_time_ns()
        end_ns = time.perf_counter_ns()
        counters_end = self._counters()

        finished = [req for req in batch.reqs if req.finished()]
        metadata["finished_request_count"] = len(finished)
        metadata["finished_request_ids"] = [req.rid for req in finished]

        # Only an observed successful sync in this execution window can bound readiness.
        ready_proxy = None
        if (
            counters_end.last_sync_result == 0
            and self._execution_start_ns <= counters_end.last_sync_start_ns
            and counters_end.last_sync_end_ns <= end_ns
        ):
            ready_proxy = {
                "execution_start_ns": self._execution_start_ns,
                "submitted_ns": self._submission_ns,
                "sync_start_ns": counters_end.last_sync_start_ns,
                "sync_end_ns": counters_end.last_sync_end_ns,
                "sync_cpu_end_ns": counters_end.last_sync_cpu_end_ns,
                "sync_probe_cpu_ns": counters_end.last_sync_probe_cpu_ns,
                "sync_result": counters_end.last_sync_result,
                "cpu_after_sync_nonwaiting_ns": cpu_end_ns
                - counters_end.last_sync_cpu_end_ns
                - (counters_end.probe_ns - counters_end.last_sync_probe_cpu_ns),
            }

        self._write(
            "publish",
            metadata,
            start_ns,
            end_ns,
            {
                "publish": CpuPart(
                    cpu_end_ns - cpu_start_ns,
                    counters_end.wait_ns - counters_start.wait_ns,
                    counters_end.probe_ns - counters_start.probe_ns,
                    counters_end.wait_wall_ns - counters_start.wait_wall_ns,
                )
            },
            ready_proxy,
        )

        self._finished_ids.update(req.rid for req in finished)

        # Workers may use os._exit; publish persists the last declared batch.
        # No JSON serialization or file I/O occurs between measured phases.
        if self._finished_ids == self.request_ids:
            self.close()

        return result
