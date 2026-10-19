"""Measure isolated native HCCL operations from declared independent work domains."""

from __future__ import annotations

import argparse
import importlib.metadata
import os
import socket
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.artifacts import AscendCaptureArtifacts
from markov_collection.adapters.runtimes.ascend.environment import RuntimeEnvironment
from markov_collection.adapters.runtimes.pytorch.profiling import TorchProfilerMarkers
from markov_collection.artifacts.json_file import read_json, write_json
from markov_collection.instrumentation.recording import Recorder
from markov_collection.instrumentation.session import CaptureSession


class CollectiveWorker:
    def __init__(self, rank: int, prepared: dict[str, Any], directory: Path, port: int) -> None:
        self.rank = rank
        self.prepared = prepared
        self.directory = directory
        self.port = port

    def run(self) -> None:
        import torch
        import torch.distributed as distributed
        import torch_npu

        world_size = len(self.prepared["environment"]["devices"])
        torch.npu.set_device(self.rank)
        distributed.init_process_group(
            backend="hccl",
            init_method=f"tcp://127.0.0.1:{self.port}",
            rank=self.rank,
            world_size=world_size,
        )

        recorder = Recorder(self.directory / "observations", self.rank, TorchProfilerMarkers())
        completions = []
        try:
            # Establish the communicator before profiling; setup barriers remain
            # outside operation scopes and are never communication cost samples.
            distributed.barrier()
            torch.npu.synchronize()
            steps = len(self.prepared["measurement_grid"]) * (self.prepared["repetitions"] + 1)
            with torch_npu.profiler.profile(
                activities=[torch_npu.profiler.ProfilerActivity.CPU, torch_npu.profiler.ProfilerActivity.NPU],
                schedule=torch_npu.profiler.schedule(wait=0, warmup=0, active=steps, repeat=1),
                record_shapes=True,
                with_stack=False,
                on_trace_ready=torch_npu.profiler.tensorboard_trace_handler(str(self.directory / "native_profile")),
                experimental_config=torch_npu.profiler._ExperimentalConfig(
                    profiler_level=torch_npu.profiler.ProfilerLevel.Level1,
                    export_type=torch_npu.profiler.ExportType.Text,
                    data_simplification=False,
                ),
            ) as profiler:
                for point in self.prepared["measurement_grid"]:
                    with recorder.scope("setup", {"stage": "setup", "grid": point["id"]}):
                        tensor = torch.ones(point["element_count"], dtype=torch.bfloat16, device=f"npu:{self.rank}")
                        output = (
                            torch.empty(point["element_count"] * world_size, dtype=torch.bfloat16, device=tensor.device)
                            if point["operation"] == "hcom_allGather"
                            else None
                        )

                    for repetition in range(-1, self.prepared["repetitions"]):
                        stage = "warmup" if repetition == -1 else "measurement"
                        with recorder.scope("setup", {"stage": "setup", "grid": point["id"]}):
                            distributed.barrier()
                            torch.npu.synchronize()

                        metadata = {"stage": stage, "grid": point["id"], "repetition": repetition, **point}
                        with recorder.scope("collective", metadata):
                            if point["operation"] == "hcom_allReduce":
                                distributed.all_reduce(tensor, op=distributed.ReduceOp.SUM)
                            else:
                                distributed.all_gather_into_tensor(output, tensor)

                            # This benchmark explicitly isolates operation completion.
                            # It does not insert synchronization in a serving capture.
                            torch.npu.synchronize()

                        completions.append({"grid": point["id"], "stage": stage, "repetition": repetition})
                        profiler.step()

            write_json(
                self.directory / f"rank-{self.rank}-environment.json",
                {
                    "rank": self.rank,
                    "pid": os.getpid(),
                    "cpu_affinity": sorted(os.sched_getaffinity(0)),
                    "logical_device": self.rank,
                    "world_size": world_size,
                    "runtime": RuntimeEnvironment().describe(),
                    "backend": distributed.get_backend(),
                },
            )

            write_json(self.directory / f"rank-{self.rank}-completions.json", completions)
        finally:
            recorder.close()
            distributed.destroy_process_group()


def run_worker(rank: int, prepared: dict[str, Any], directory: Path, port: int) -> None:
    CollectiveWorker(rank, prepared, directory, port).run()


class NativeCollectiveSampling(CaptureSession):
    def run(self) -> None:
        import torch.multiprocessing as multiprocessing

        if not self.prepared["permission"]["allow_run"]:
            raise ValueError("performance collection permission is disabled")

        artifacts = AscendCaptureArtifacts(self.prepared, self.directory)
        write_json(self.directory / "prepared_input.json", self.prepared)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]

        world_size = len(self.prepared["environment"]["devices"])
        multiprocessing.spawn(run_worker, args=(self.prepared, self.directory, port), nprocs=world_size, join=True)

        write_json(
            self.directory / "native_environment.json",
            {
                "framework": self.prepared["framework"],
                "framework_version": importlib.metadata.version(self.prepared["framework"]),
                "configuration": self.prepared["configuration"],
                "deployment": self.prepared["environment"],
                "runtime": RuntimeEnvironment().describe(),
                "backend": "hccl",
                "workers": [f"rank-{rank}-environment.json" for rank in range(world_size)],
                "profiler_enabled": True,
                "protocol": "per-condition warmup; rank barrier and device drain before each isolated operation",
            },
        )

        write_json(
            self.directory / "measurement_completions.json",
            [f"rank-{rank}-completions.json" for rank in range(world_size)],
        )
        artifacts.publish()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    prepared = read_json(arguments.input)

    NativeCollectiveSampling(prepared, arguments.output).run()


if __name__ == "__main__":
    main()
