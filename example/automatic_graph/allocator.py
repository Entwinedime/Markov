#!/usr/bin/env python3
"""Observe native paged allocation, deferred reuse and per-request loops."""

from __future__ import annotations

import argparse
import json
from contextlib import nullcontext
from pathlib import Path

import torch
import torch_npu
from markov_collection.adapters.frameworks.sglang.allocation import SGLangAllocation
from markov_collection.adapters.runtimes.ascend.synchronization import SynchronizationObservation
from markov_collection.adapters.runtimes.pytorch.observation import OperatorObservation, TorchMarkers
from markov_collection.artifacts import write_json
from markov_collection.instrumentation.recording import Recording


class AllocationExample:
    def __init__(self, output: Path, device: str, page_size: int, need_sort: bool) -> None:
        if page_size <= 0:
            raise ValueError("page size must be positive")
        self.output = output
        self.device = device
        self.recording = Recording(TorchMarkers())
        self.observer = OperatorObservation(self.recording)
        self.synchronization = SynchronizationObservation(self.recording.markers)
        self.allocation = SGLangAllocation(self.observer, page_size, device, need_sort)

    def run(self) -> None:
        (self.output / "runtime").mkdir(parents=True)
        with torch.inference_mode():
            self.allocation.warmup()

            # Warm the native allocation paths before collecting baseline and marked executions.
            profile = (
                torch_npu.profiler.profile(
                    activities=[torch_npu.profiler.ProfilerActivity.CPU, torch_npu.profiler.ProfilerActivity.NPU],
                    record_shapes=False,
                    with_stack=False,
                    experimental_config=torch_npu.profiler._ExperimentalConfig(
                        profiler_level=torch_npu.profiler.ProfilerLevel.Level1
                    ),
                    on_trace_ready=torch_npu.profiler.tensorboard_trace_handler(str(self.output / "native")),
                )
                if self.device == "npu"
                else nullcontext()
            )
            with profile:
                self.allocation.capture()

        self.allocation.declare(self.recording.regions, self.recording.operations, self.recording.values)

        members = [
            region["metadata"]["parameters"]
            for region in self.recording.regions
            if region["metadata"].get("entrypoint") == "allocation_member"
        ]
        environment = {
            "purpose": "native_allocator_loop_mechanism", "device": self.device, "rank": 0,
            "page_size": self.allocation.page_size, "need_sort": self.allocation.need_sort,
        }
        self.recording.write(self.output / "observations.json", environment)
        self.synchronization.write(self.output / "runtime")
        summary = {
            **environment, "members": len(members), "operations": len(self.recording.operations),
            "branch_patterns": sorted({
                tuple(member[name] > 0 for name in ("head_tokens", "full_pages", "tail_tokens")) for member in members
            }),
        }
        write_json(self.output / "summary.json", summary, indent=2)
        print(json.dumps(summary))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--device", choices=("cpu", "npu"), default="npu")
    parser.add_argument("--page-size", type=int, default=128)
    parser.add_argument("--need-sort", action="store_true")
    arguments = parser.parse_args()
    AllocationExample(arguments.output, arguments.device, arguments.page_size, arguments.need_sort).run()
