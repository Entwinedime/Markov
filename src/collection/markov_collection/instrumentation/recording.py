"""Own observation identities and semantic scopes without owning native tensors."""

from __future__ import annotations

import threading
import time
from contextvars import ContextVar
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import write_json


class Scope:
    def __init__(self, recording: Recording, role: str, metadata: dict[str, Any]) -> None:
        self.recording = recording
        self.metadata = metadata
        self.role = role

    def __enter__(self) -> Scope:
        parent = self.recording.current_scope.get()
        self.identity = len(self.recording.regions)
        self.record = {
            "id": self.identity,
            "parent": parent,
            "role": self.role,
            "metadata": self.metadata,
            "operation_begin": len(self.recording.operations),
            "started_ns": time.perf_counter_ns(),
            "thread": threading.get_native_id(),
            "started_cpu_ns": time.thread_time_ns(),
        }
        self.recording.regions.append(self.record)

        # Restore this exact parent token on exit so nested calls retain their original owner.
        self.token = self.recording.current_scope.set(self.identity)
        self.marker = self.recording.markers.scope(f"markov.region/{self.identity}")
        self.marker.__enter__()
        return self

    def __exit__(self, kind: Any, error: Any, traceback: Any) -> None:
        try:
            self.marker.__exit__(kind, error, traceback)
        finally:
            self.record["ended_ns"] = time.perf_counter_ns()
            self.record["ended_cpu_ns"] = time.thread_time_ns()
            self.record["completed"] = error is None

            self.recording.current_scope.reset(self.token)


class Recording:
    def __init__(self, markers: Any) -> None:
        self.markers = markers
        self.current_scope: ContextVar[int | None] = ContextVar("markov_scope", default=None)

        self.regions: list[dict[str, Any]] = []
        self.operations: list[dict[str, Any]] = []
        self.values: list[dict[str, Any]] = []
        self.modules: list[dict[str, Any]] = []
        self.batches: list[dict[str, Any]] = []

    def scope(self, role: str, **metadata: Any) -> Scope:
        return Scope(self, role, metadata)

    def document(self, environment: dict[str, Any]) -> dict[str, Any]:
        return {
            "environment": environment,
            "modules": self.modules,
            "batches": self.batches,
            "regions": self.regions,
            "values": self.values,
            "operations": self.operations,
        }

    def write(self, path: Path, environment: dict[str, Any]) -> None:
        write_json(path, self.document(environment))
