#!/usr/bin/env python3
"""Observe native single-worker collectives with a logical TP layout and dynamic input rows."""

from __future__ import annotations

import argparse
from pathlib import Path

import torch
import torch_npu  # noqa: F401
from markov_collection.adapters.frameworks.sglang.parallel import CollectiveCall, SingleDeviceShard
from markov_collection.adapters.runtimes.pytorch.observation import OperatorObservation, TorchMarkers
from markov_collection.instrumentation.recording import Recording
from sglang.srt.distributed.parallel_state import (
    destroy_distributed_environment,
    destroy_model_parallel,
    get_tp_group,
    init_distributed_environment,
    initialize_model_parallel,
)
from sglang.srt.plugins.hook_registry import HookRegistry
from sglang.srt.utils.network import get_free_port


class CollectiveExample:
    def __init__(self, output: Path, size: int, rank: int) -> None:
        self.output = output
        self.recording = Recording(TorchMarkers())
        self.observer = OperatorObservation(self.recording)
        self.shard = SingleDeviceShard(size, rank, self.observer.values)

    def work(self, tensor: torch.Tensor) -> torch.Tensor:
        group = get_tp_group()
        tensor = group.all_reduce(tensor)
        return group.all_gather(tensor, dim=-1)

    def run(self) -> None:
        self.shard.install_transport()
        HookRegistry.apply_hooks()
        init_distributed_environment(
            world_size=1,
            rank=0,
            local_rank=0,
            distributed_init_method=f"tcp://127.0.0.1:{get_free_port()}",
            backend="hccl",
        )
        try:
            initialize_model_parallel(1, backend="hccl")
            for rows in (2, 3):
                tensor = torch.ones((rows, 16), device="npu")
                self.observer.begin_batch()
                self.observer.values.bind(tensor, {0: "M"})
                batch = len(self.recording.batches)
                self.recording.batches.append(
                    {"id": batch, "phase": "prefill", "rank": self.shard.rank, "parameters": {"M": rows, "B": 1}}
                )
                with self.recording.scope("ModelSubmit", batch=batch), self.observer:
                    result = self.shard.execute(self.work, tensor)
                torch.npu.synchronize()
                print(
                    {"rows": rows, "logical_tp": self.shard.size, "rank": self.shard.rank, "output": list(result.shape)}
                )
            CollectiveCall.bind_transports(self.recording.regions, self.recording.operations, self.recording.values)
            self.recording.write(
                self.output / "observations.json",
                {"purpose": "native_collective_mechanism", "physical_tp": 1, "logical_tp": self.shard.size},
            )
        finally:
            self.shard.communicators.clear()
            destroy_model_parallel()
            destroy_distributed_environment()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tp-size", type=int, default=2)
    parser.add_argument("--rank", type=int, default=0)
    arguments = parser.parse_args()
    CollectiveExample(arguments.output, arguments.tp_size, arguments.rank).run()
