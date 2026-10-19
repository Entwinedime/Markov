"""Associate synchronous driver copies with explicitly observed native batches."""

from __future__ import annotations

import ctypes
import json
import os
from collections import defaultdict
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import write_json
from markov_collection.artifacts.sampling_domain import SamplingDomain


class TransferSampling:
    def __init__(self, directory: Path) -> None:
        self._native = ctypes.CDLL(None)
        self._native.markov_native_begin_transfer_sampling.argtypes = [ctypes.c_char_p]
        self._native.markov_native_begin_transfer_sampling.restype = None
        self._native.markov_native_transfer_scope.argtypes = [ctypes.c_uint64]
        self._native.markov_native_transfer_scope.restype = ctypes.c_uint64
        self._native.markov_native_flush_transfer_sampling.argtypes = []
        self._native.markov_native_flush_transfer_sampling.restype = ctypes.c_int
        self._native.markov_native_begin_transfer_sampling(os.fsencode(directory))

    @contextmanager
    def scope(self, identity: int) -> Iterator[None]:
        previous = self._native.markov_native_transfer_scope(identity)
        try:
            yield
        finally:
            self._native.markov_native_transfer_scope(previous)

    def flush(self) -> None:
        if self._native.markov_native_flush_transfer_sampling() != 0:
            raise RuntimeError("native transfer sampling flush failed")


class TransferMeasurements:
    def __init__(self, scopes: list[Path], workers: list[Path], domain: SamplingDomain) -> None:
        self.scope_paths = scopes
        self.worker_paths = workers
        self.domain = domain

    def consume_journals(self) -> None:
        # Normalized units retain measured copies and source coordinates. The
        # capture owner calls this after publishing the manifest that owns them.
        for path in self.worker_paths:
            path.unlink()

    def publish(self, output: Path) -> dict[str, Any]:
        scopes = {}
        for path in self.scope_paths:
            with path.open(encoding="utf-8") as stream:
                for line in stream:
                    record = json.loads(line)
                    identity = (record["pid"], record["scope_id"])
                    if identity in scopes:
                        raise ValueError("duplicate native transfer observation scope")
                    scopes[identity] = record
                    if record["completed"] and record["metadata"] is not None and not record["control"]:
                        self.domain.observe(record["rank"], record["step"], record["metadata"])

        pairs: dict[tuple[int, int], dict[str, Any]] = defaultdict(dict)
        for path in self.worker_paths:
            with path.open(encoding="utf-8") as stream:
                for line_number, line in enumerate(stream, start=1):
                    record = json.loads(line)
                    pair = pairs[(record["pid"], record["correlation_id"])]
                    phase = record["phase"]
                    if phase not in {"enqueue", "worker"} or phase in pair:
                        raise ValueError(f"invalid or duplicate queue phase at {path}:{line_number}")
                    pair[phase] = {
                        **record,
                        "source": {"path": os.path.relpath(path, output.parent), "line": line_number},
                    }

        units = []
        gaps = []
        directions = ("host_to_host", "host_to_device", "device_to_host", "device_to_device")
        for identity, pair in pairs.items():
            reason = None
            if set(pair) != {"enqueue", "worker"}:
                reason = "queue_pair_incomplete"
            elif any(record["result"] != 0 for record in pair.values()):
                reason = "queue_call_failed"
            elif pair["enqueue"]["scope_id"] != pair["worker"]["scope_id"]:
                reason = "queue_scope_mismatch"
            else:
                scope = scopes.get((identity[0], pair["enqueue"]["scope_id"]))
                if scope is None or not scope["completed"]:
                    reason = "completed_observation_scope_missing"
                elif scope["metadata"] is None:
                    reason = "native_batch_metadata_missing"

            if reason is not None:
                gaps.append({"identity": list(identity), "reason": reason})
                continue

            worker = pair["worker"]
            for index, copy in enumerate(worker["driver_copies"]):
                source = {**worker["source"], "leaf": index}
                boundaries = copy["boundaries"]
                reason = None
                if copy["result"] != 0:
                    reason = "driver_copy_failed"
                elif len(boundaries) != 2:
                    reason = "driver_copy_boundary_incomplete"
                elif not worker["start_ns"] <= boundaries[0]["wall_ns"] <= boundaries[1]["wall_ns"] <= worker["end_ns"]:
                    reason = "driver_copy_outside_worker_window"
                elif copy["direction"] not in range(len(directions)):
                    reason = "driver_copy_direction_unknown"

                if reason is not None:
                    gaps.append({"identity": list(identity), "source": source, "reason": reason})
                    continue

                units.append(
                    {
                        "stage": self.domain.stage(scope["metadata"]),
                        "rank": scope["rank"],
                        "pid": worker["pid"],
                        "tid": worker["tid"],
                        "transfer": {
                            "bytes": copy["bytes"],
                            "direction": directions[copy["direction"]],
                            "execution": "synchronous_host_driver",
                        },
                        "boundaries": boundaries,
                        # The driver's complete wall interval includes I/O stalls.
                        "duration_ns": boundaries[1]["wall_ns"] - boundaries[0]["wall_ns"],
                        "result": copy["result"],
                        "source": source,
                    }
                )

        coverage = {"queue_pairs": len(pairs), "transfer_units": len(units), "gaps": gaps}
        write_json(
            output,
            {"kind": "cpu_worker_units", "unit": "ns", "transfer_units": units, "coverage": coverage},
            compact=True,
        )
        return coverage
