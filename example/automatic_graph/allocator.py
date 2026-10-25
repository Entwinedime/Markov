#!/usr/bin/env python3
"""Observe native large-page allocation and its per-request loop without rewriting it."""

from __future__ import annotations

import argparse
import json
import sys
from contextlib import nullcontext
from pathlib import Path

import torch
import torch_npu
from markov_collection.adapters.frameworks.sglang.allocation import SGLangAllocation
from markov_collection.adapters.runtimes.ascend.synchronization import SynchronizationObservation
from markov_collection.adapters.runtimes.pytorch.observation import OperatorObservation, TorchMarkers
from markov_collection.artifacts.json_file import write_json
from markov_collection.instrumentation.recording import Recording


class AllocationExample:
    page_size = 128

    def __init__(self, output: Path, device: str) -> None:
        self.output = output
        self.device = device
        self.recording = Recording(TorchMarkers())
        self.observer = OperatorObservation(self.recording)
        self.synchronization = SynchronizationObservation(self.recording.markers)
        self.allocation = SGLangAllocation(self.observer, self.page_size, device, False)

    def run(self) -> None:
        (self.output / "runtime").mkdir(parents=True)
        previous_trace = sys.gettrace()
        with torch.inference_mode():
            self.allocation.warmup()

            # Warm both finite points before collecting the baseline and marked native executions.
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
        assert len(members) == sum(map(len, self.allocation.points())) and sys.gettrace() is previous_trace
        self.recording.write(
            self.output / "observations.json", {"purpose": "native_allocator_loop_mechanism", "device": self.device}
        )
        self.synchronization.write(self.output / "runtime")
        summary = {"device": self.device, "members": members, "operations": len(self.recording.operations)}
        write_json(self.output / "summary.json", summary, indent=2)
        print(json.dumps(summary))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--device", choices=("cpu", "npu"), default="npu")
    arguments = parser.parse_args()
    AllocationExample(arguments.output, arguments.device).run()
