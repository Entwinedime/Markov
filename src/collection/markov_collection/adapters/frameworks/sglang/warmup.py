"""Define the bounded SGLang warmup recipe and observe its native completion."""

from __future__ import annotations

import json
import os
from collections.abc import Callable
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.torch_npu.workspace import WorkspaceCapture


class SGLangWarmup:
    def __init__(self, directory: Path, prefix: str, *, source_rank: int | None = None) -> None:
        self.directory = directory
        self.prefix = prefix
        self._source_rank = source_rank
        self._cohort_ends: dict[int, int] | None = None
        self._snapshot_capacity: int | None = None
        directory.mkdir(parents=True, exist_ok=True)

    @staticmethod
    def points(page_size: int, capacity: int) -> list[dict[str, Any]]:
        # The allocator specializes on rounded batch and token extents. The
        # client and workers interpret the same bounded recipe.
        counts = sorted({1, capacity, *(1 << bit for bit in range(capacity.bit_length()))})
        lengths = {page_size - 1, page_size}
        length = 1
        while length <= page_size:
            lengths.add(length)
            length *= 2

        points = []
        for count in counts:
            for length in sorted(lengths):
                points.append(
                    {
                        "request_capacity": count,
                        "mechanism": "cold_lifecycle",
                        "requests": [
                            {"prompt_tokens": length, "output_tokens": 3, "release_after_tokens": 0}
                            for _ in range(count)
                        ],
                    }
                )

            if count < 2:
                continue

            # An existing KV page changes the allocator's pointer specialization.
            # Warm its live-admission path at the same bounded token extents.
            for length in sorted(lengths):
                points.append(
                    {
                        "request_capacity": count,
                        "mechanism": "running_prefill",
                        "requests": [{"prompt_tokens": 1, "output_tokens": 3, "release_after_tokens": 0}]
                        + [
                            {"prompt_tokens": length, "output_tokens": 3, "release_after_tokens": 1}
                            for _ in range(count - 1)
                        ],
                    }
                )
        return points

    def execute(self, original: Callable[..., Any], scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:
        if not batch.reqs or not all(request.rid.startswith(self.prefix) for request in batch.reqs):
            return original(scheduler, batch, *args, **kwargs)

        if self._cohort_ends is None:
            points = self.points(scheduler.server_args.page_size, scheduler.max_running_requests)
            self._cohort_ends = {
                index: point["request_capacity"]
                for index, point in enumerate(points)
                if index + 1 == len(points) or point["request_capacity"] != points[index + 1]["request_capacity"]
            }
        index = int(batch.reqs[0].rid[len(self.prefix) :].split("/", 1)[0])
        self._snapshot_capacity = self._cohort_ends.get(index)

        record = {
            "point": index,
            "rank": self._rank(scheduler),
            "pid": os.getpid(),
            "phase": "prefill" if batch.forward_mode.is_extend() else "decode",
            "requests": [request.rid for request in batch.reqs],
            "input_tokens": [len(request.origin_input_ids) for request in batch.reqs],
            "running_requests": sum(not request.finished() for request in scheduler.running_batch.reqs),
        }
        result = original(scheduler, batch, *args, **kwargs)
        with (self.directory / f"{os.getpid()}.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(record) + "\n")
        return result

    def reset(self, original: Callable[..., Any], scheduler: Any, *args: Any, **kwargs: Any) -> Any:
        result = original(scheduler, *args, **kwargs)
        if result and self._snapshot_capacity is not None:
            # The native deferred reset establishes idleness. Observe the actual
            # retained workspace after each complete request-capacity cohort.
            directory = self.directory.parent / "warmup_state" / str(self._snapshot_capacity)
            WorkspaceCapture(directory, self._rank(scheduler)).begin()
            self._snapshot_capacity = None
        return result

    def _rank(self, scheduler: Any) -> int:
        return scheduler.ps.tp_rank if self._source_rank is None else self._source_rank

    @staticmethod
    def batches(directory: Path) -> list[dict[str, Any]]:
        records = []
        for path in sorted(directory.glob("*.jsonl")):
            with path.open(encoding="utf-8") as stream:
                records.extend(json.loads(line) for line in stream)
        return records
