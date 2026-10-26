#!/usr/bin/env python3
"""Observe native paged allocation/release, including CPU unique and device pool updates."""

from __future__ import annotations

import argparse
import time
from contextlib import nullcontext
from pathlib import Path
from types import MethodType

import torch
import torch_npu
from markov_collection.adapters.frameworks.sglang.state import PageMergeCall, PageReleaseCall, PageReleaseGroupCall
from markov_collection.adapters.runtimes.pytorch.observation import OperatorObservation, TorchMarkers
from markov_collection.artifacts.json_file import write_json
from markov_collection.instrumentation.recording import Recording
from sglang.srt.hardware_backend.npu.allocator_npu import NPUPagedTokenToKVPoolAllocator


class PageStateExample:
    points = (
        (1, False, False),
        (129, False, False),
        (256, False, False),
        (1, True, False),
        (0, False, False),
        (129, False, True),
    )
    groups = ((129,), (1, 128, 257))

    def __init__(self, output: Path) -> None:
        self.output = output
        self.recording = Recording(TorchMarkers())
        self.observer = OperatorObservation(self.recording)

    def execute(self, tokens: int, observed: bool, deferred: bool = False, need_sort: bool = False) -> None:

        # The native allocator owns the page list; no replica of its allocation/free implementation is used.
        pool = NPUPagedTokenToKVPoolAllocator(4096, 128, torch.bfloat16, "npu", None, need_sort)
        indices = pool.alloc(((tokens + 127) // 128) * 128)[:tokens]
        if deferred:
            pending = pool.alloc(128)[:1]
            pool.free_group_begin()
            pool.free(pending)
        batch = len(self.recording.batches)
        self.recording.batches.append(
            {
                "id": batch,
                "phase": "decode",
                "rank": 0,
                "parameters": {
                    "M": 1,
                    "B": 1,
                    "P": 128,
                    "K": 4096,
                    "release_immediate": int(not deferred),
                    "need_sort": int(need_sort),
                },
                "members": [{"new_tokens": 1, "context_tokens": max(tokens - 1, 0)}],
                "observation": "source" if observed else "baseline",
            }
        )
        if observed:
            self.observer.begin_batch()
        scope = (
            self.recording.scope("ResultProcessed", batch=batch)
            if observed
            else TorchMarkers.scope(f"markov.baseline/{batch}")
        )

        # Keep native group/free lifecycle; wrappers only expose the two call boundaries.
        with scope, self.observer if observed else nullcontext():
            if observed:
                with self.observer.values.metadata():
                    call = PageReleaseCall(
                        self.recording, self.observer.values, NPUPagedTokenToKVPoolAllocator.free, pool
                    )
                call(pool, indices)
                if need_sort:
                    PageMergeCall(self.observer.values, pool.merge_and_sort_free, pool)()
            else:
                pool.free(indices)
                if need_sort:
                    pool.merge_and_sort_free()
        torch.npu.synchronize()

    def execute_group(self, tokens: tuple[int, ...], observed: bool) -> None:
        pool = NPUPagedTokenToKVPoolAllocator(4096, 128, torch.bfloat16, "npu", None, False)
        indices = [pool.alloc(((length + 127) // 128) * 128)[:length] for length in tokens]
        pool.free_group_begin()
        for value in indices:
            pool.free(value)

        batch = len(self.recording.batches)
        self.recording.batches.append(
            {
                "id": batch,
                "phase": "decode",
                "rank": 0,
                "parameters": {"M": len(tokens), "B": len(tokens)},
                "members": [{"new_tokens": 1, "context_tokens": length - 1} for length in tokens],
                "observation": "source" if observed else "baseline",
            }
        )
        if observed:
            self.observer.begin_batch()
        scope = (
            self.recording.scope("ResultProcessed", batch=batch)
            if observed
            else TorchMarkers.scope(f"markov.baseline/{batch}")
        )
        with scope, self.observer if observed else nullcontext():
            if observed:
                pool.free = MethodType(
                    PageReleaseCall(self.recording, self.observer.values, NPUPagedTokenToKVPoolAllocator.free, pool),
                    pool,
                )
                try:
                    PageReleaseGroupCall(
                        self.recording, self.observer.values, NPUPagedTokenToKVPoolAllocator.free_group_end, pool
                    )(pool)
                finally:
                    del pool.free
            else:
                pool.free_group_end()
        torch.npu.synchronize()

    def run(self) -> None:
        self.output.mkdir(parents=True, exist_ok=True)
        started = time.perf_counter_ns()
        for tokens, deferred, need_sort in self.points:
            self.execute(tokens, False, deferred, need_sort)
        for tokens in self.groups:
            self.execute_group(tokens, False)
        self.recording.batches.clear()
        warmed = time.perf_counter_ns()
        with torch_npu.profiler.profile(
            activities=[torch_npu.profiler.ProfilerActivity.CPU, torch_npu.profiler.ProfilerActivity.NPU],
            with_stack=False,
            record_shapes=False,
            on_trace_ready=torch_npu.profiler.tensorboard_trace_handler(str(self.output / "native")),
        ):
            for tokens, deferred, need_sort in self.points:
                self.execute(tokens, False, deferred, need_sort)
                self.execute(tokens, True, deferred, need_sort)
            for tokens in self.groups:
                self.execute_group(tokens, False)
                self.execute_group(tokens, True)
        self.recording.write(
            self.output / "observations.json", {"purpose": "native_allocator_mechanism", "torch": torch.__version__}
        )
        write_json(
            self.output / "summary.json",
            {"warmup_ns": warmed - started, "observation_export_ns": time.perf_counter_ns() - warmed},
            indent=2,
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    PageStateExample(parser.parse_args().output).run()
