"""Project queue observations onto non-overlapping thread CPU measurements."""

from __future__ import annotations

import json
import os
from collections import Counter, defaultdict
from itertools import pairwise
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.torch_npu.queue import EVENT_NAMES
from markov_collection.artifacts.json_file import write_json


class CpuWorkerUnits:
    def __init__(self, phases: list[Path], workers: list[Path]) -> None:
        self.phase_paths = phases
        self.worker_paths = workers

    def publish(self, path: Path) -> dict[str, Any]:
        scopes = []
        lanes: dict[tuple[int, int], list[int]] = defaultdict(list)
        for source in self.phase_paths:
            with source.open(encoding="utf-8") as stream:
                for line_number, line in enumerate(stream, start=1):
                    record = json.loads(line)
                    lanes[(record["pid"], record["tid"])].append(len(scopes))
                    scopes.append({**record, "source": self._source(path, source, line_number)})

        pairs: dict[tuple[int, int], dict[str, Any]] = defaultdict(dict)
        for source in self.worker_paths:
            with source.open(encoding="utf-8") as stream:
                for line_number, line in enumerate(stream, start=1):
                    record = json.loads(line)
                    pair = pairs[(record["pid"], record["correlation_id"])]
                    phase = record["phase"]
                    if phase not in {"enqueue", "worker"} or phase in pair:
                        raise ValueError(f"invalid or duplicate queue phase at {source}:{line_number}")
                    pair[phase] = {**record, "source": self._source(path, source, line_number)}

        units = []
        gaps = []
        counts: Counter[str] = Counter()
        for identity, pair in pairs.items():
            if set(pair) != {"enqueue", "worker"}:
                gaps.append({"identity": list(identity), "reason": "queue_pair_incomplete"})
                continue

            enqueue, worker = pair["enqueue"], pair["worker"]
            if enqueue["result"] != 0 or worker["result"] != 0:
                gaps.append({"identity": list(identity), "reason": "queue_call_failed"})
                continue

            # Enclosing scopes live on the enqueue thread. Queue identity, not
            # overlap or proximity, then carries that scope to the worker.
            enclosing = [
                index
                for index in lanes[(enqueue["pid"], enqueue["tid"])]
                if scopes[index]["start_ns"] <= enqueue["start_ns"]
                and enqueue["end_ns"] <= scopes[index]["end_ns"]
            ]
            if not enclosing:
                gaps.append({"identity": list(identity), "reason": "enqueue_scope_missing"})
                continue

            enclosing.sort(key=lambda index: scopes[index]["end_ns"] - scopes[index]["start_ns"])
            scope = scopes[enclosing[0]]
            if any(
                not (
                    other["start_ns"] <= scope["start_ns"]
                    and scope["end_ns"] <= other["end_ns"]
                    and (scope["start_ns"], scope["end_ns"]) != (other["start_ns"], other["end_ns"])
                )
                for other in (scopes[index] for index in enclosing[1:])
            ):
                gaps.append({"identity": list(identity), "reason": "enqueue_scope_ambiguous"})
                continue

            operation = worker["operation"]
            if operation is None:
                operation = EVENT_NAMES.get(worker["task_type"])

            units.append(
                {
                    "pid": identity[0],
                    "correlation_id": identity[1],
                    "scope": enclosing[0],
                    "operation": operation,
                    "task_type": worker["task_type"],
                    "enqueue_source": enqueue["source"],
                    "worker_source": worker["source"],
                    "enqueue_tid": enqueue["tid"],
                    "worker_tid": worker["tid"],
                    "enqueue": self._segments(enqueue),
                    "worker": self._segments(worker),
                }
            )
            counts[scope["stage"]] += 1

        summary = {"queue_pairs": len(pairs), "units": len(units), "stages": dict(counts), "gaps": gaps}
        write_json(
            path,
            {
                "kind": "cpu_worker_units",
                "unit": "ns",
                "measurement": "adjacent points on one thread; scopes are inclusive provenance, not additional costs",
                "readiness": "needs_data",
                "scopes": scopes,
                "units": units,
                "coverage": summary,
            },
            compact=True,
        )
        return summary

    @staticmethod
    def _source(output: Path, source: Path, line: int) -> dict[str, Any]:
        return {"path": os.path.relpath(source, output.parent), "line": line}

    @staticmethod
    def _segments(record: dict[str, Any]) -> dict[str, Any]:
        points = [
            {
                "boundary": "begin",
                "thread_cpu_ns": record["cpu_start_ns"],
                "wait_cpu_ns": record["wait_cpu_start_ns"],
                "probe_cpu_ns": record["probe_cpu_start_ns"],
            }
        ]
        stack = []
        for point in record["runtime_points"]:
            if point["entering"]:
                stack.append(point["api"])
            elif not stack or stack.pop() != point["api"]:
                raise ValueError("worker runtime boundaries are not properly nested")

            points.append({"boundary": "enter" if point["entering"] else "exit", **point})
        if stack:
            raise ValueError("worker runtime call has no exit boundary")

        points.append(
            {
                "boundary": "end",
                "thread_cpu_ns": record["cpu_end_ns"],
                "wait_cpu_ns": record["wait_cpu_end_ns"],
                "probe_cpu_ns": record["probe_cpu_end_ns"],
            }
        )

        segments = []
        for begin, end in pairwise(points):
            # Adjacent intervals partition the observation exactly, including
            # nested API calls. No parent API total is charged a second time.
            values = {field: end[field] - begin[field] for field in ("thread_cpu_ns", "wait_cpu_ns", "probe_cpu_ns")}
            if any(value < 0 for value in values.values()):
                raise ValueError("worker CPU counters move backwards")
            segments.append(values)

        return {
            "boundaries": [{key: point[key] for key in ("boundary", "api") if key in point} for point in points],
            "segments": segments,
        }
