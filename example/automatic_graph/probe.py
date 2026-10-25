#!/usr/bin/env python3
"""Acquire bounded operator/shape evidence on the actual inference runtime."""

from __future__ import annotations

import argparse
import json
import time
from contextlib import nullcontext
from pathlib import Path

import torch
import torch_npu
from markov_collection.adapters.runtimes.ascend.layouts import AscendLayouts
from markov_collection.adapters.runtimes.ascend.synchronization import SynchronizationObservation
from markov_collection.adapters.runtimes.pytorch.concatenation import ConcatenationObservation, GroupRepetition
from markov_collection.adapters.runtimes.pytorch.indexing import TensorAssignmentCall
from markov_collection.adapters.runtimes.pytorch.modules import ModuleObservation
from markov_collection.adapters.runtimes.pytorch.observation import OperatorObservation, TorchMarkers
from markov_collection.artifacts.json_file import write_json
from markov_collection.instrumentation.recording import Recording


class Projection(torch.nn.Module):
    def __init__(self) -> None:
        super().__init__()
        self.up = torch.nn.Linear(16, 32, bias=False)
        self.down = torch.nn.Linear(32, 16, bias=False)

    def forward(self, value: torch.Tensor) -> torch.Tensor:
        original_shape = value.shape
        value.reshape(-1, 2, 8).transpose(0, 1).contiguous()
        value = self.down(torch.nn.functional.silu(self.up(value)))
        rows = value.size(0)
        width = value.size()[-1]
        value = value.reshape(rows, -1)
        value = value.contiguous().view(-1, width)
        value = torch.reshape(value, (value.size(0), -1))
        value = self.restore_width(value, self.down.out_features)
        return value.reshape(-1, 4, 4).view(original_shape)

    def restore_width(self, value: torch.Tensor, width: int) -> torch.Tensor:
        return value.reshape(-1, width)


class MechanismProbe:
    points = ((1, 1), (2, 2), (3, 3), (16, 33))

    def __init__(self, output: Path, profile: bool) -> None:
        self.output = output
        self.profile = profile
        self.model = torch.nn.Sequential(Projection(), torch.nn.Flatten(1), Projection()).to("npu")
        self.transfer_stream = torch.npu.Stream()
        self.recording = Recording(TorchMarkers())
        self.synchronization = SynchronizationObservation(self.recording.markers)
        self.observer = OperatorObservation(self.recording)

    def work(
        self, value: torch.Tensor, indices: torch.Tensor, buffer: torch.Tensor, members: int, observed: bool = False
    ) -> None:
        result = self.model(value)
        view = result.view(-1, 16)
        view.add_(1)
        result.clone()
        torch.cat([view] * members, dim=0)
        with self.recording.scope("Prepare") if observed else nullcontext():
            with (
                ConcatenationObservation(self.observer.values, repetition=GroupRepetition("L", "produce"))
                if observed
                else nullcontext()
            ):
                torch.cat([torch.tensor([-1], device=value.device) for _ in range(members)])

        totals = view.sum(dim=1)
        workspace = torch.empty(totals.shape, dtype=totals.dtype, device=totals.device)
        torch._foreach_copy_([workspace], [totals])
        buffer[indices] = totals
        host = torch.tensor([members], dtype=torch.int64).view(-1, 1)
        with torch.npu.stream(self.transfer_stream):
            host.to(value.device, non_blocking=True).cpu()
        torch.tensor([value.shape[0]], dtype=torch.int64, device=value.device)

    def execute(self, rows: int, members: int, observed: bool) -> None:
        value = torch.ones((rows, 16), device="npu")
        indices = torch.arange(rows, device="npu")
        buffer = torch.zeros(17, device="npu")
        batch = len(self.recording.batches)
        self.recording.batches.append(
            {
                "id": batch,
                "phase": "prefill",
                "rank": 0,
                "parameters": {"M": rows, "L": members, "R": 16},
                "members": [{"new_tokens": rows, "context_tokens": 0}],
                "observation": "source" if observed else "baseline",
            }
        )
        if observed:
            self.observer.begin_batch()
            self.observer.values.bind(value, {0: "M"})
            self.observer.values.bind(indices, {0: "M"})
            self.observer.values.bind_shape(buffer, (self.observer.values.shapes.symbol("R") + 1,))
        scope = (
            self.recording.scope("Prefill", batch=batch) if observed else TorchMarkers.scope(f"markov.baseline/{batch}")
        )
        with scope, self.observer if observed else nullcontext():
            with (
                ConcatenationObservation(self.observer.values, repetition=GroupRepetition("L", "reuse"))
                if observed
                else nullcontext()
            ):
                self.work(value, indices, buffer, members, observed)
        torch.npu.synchronize()

    def run(self) -> None:
        self.output.mkdir(parents=True, exist_ok=True)
        started = time.perf_counter_ns()
        with torch.inference_mode():
            for rows, members in self.points:
                self.work(
                    torch.ones((rows, 16), device="npu"),
                    torch.arange(rows, device="npu"),
                    torch.zeros(17, device="npu"),
                    members,
                )
            torch.npu.synchronize()
            warmed = time.perf_counter_ns()

            profile = None
            if self.profile:
                profile = torch_npu.profiler.profile(
                    activities=[torch_npu.profiler.ProfilerActivity.CPU, torch_npu.profiler.ProfilerActivity.NPU],
                    record_shapes=False,
                    with_stack=False,
                    experimental_config=torch_npu.profiler._ExperimentalConfig(
                        profiler_level=torch_npu.profiler.ProfilerLevel.Level1
                    ),
                    on_trace_ready=torch_npu.profiler.tensorboard_trace_handler(str(self.output / "native")),
                )
                profile.start()
            try:
                with ModuleObservation(self.recording, self.model, self.observer.values, {}):
                    for rows, members in self.points:
                        self.execute(rows, members, False)
                        self.execute(rows, members, True)
            finally:
                if profile is not None:
                    profile.stop()

        ended = time.perf_counter_ns()
        environment = {
            "torch": torch.__version__,
            "torch_npu": torch_npu.__version__,
            "device": "npu",
            "device_index": torch.npu.current_device(),
        }
        geometries = TensorAssignmentCall.input_geometries(self.recording.regions)
        TensorAssignmentCall.declare(self.recording.regions, self.recording.values, geometries)
        AscendLayouts.declare(self.recording.regions, self.recording.operations, self.recording.values)
        self.recording.write(self.output / "observations.json", environment)
        self.synchronization.write(self.output / "runtime")
        summary = {
            "warmup_ns": warmed - started,
            "observed_ns": ended - warmed,
            "batches": len(self.recording.batches),
            "operations": len(self.recording.operations),
            "values": len(self.recording.values),
            "shape_errors": [
                {"operation": item["id"], "name": item["name"], "reason": item["shape_error"]}
                for item in self.recording.operations
                if item["shape_error"] is not None
            ],
        }
        write_json(self.output / "probe.json", summary, indent=2)
        print(json.dumps(summary))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profile", action="store_true")
    arguments = parser.parse_args()
    MechanismProbe(arguments.output, arguments.profile).run()
