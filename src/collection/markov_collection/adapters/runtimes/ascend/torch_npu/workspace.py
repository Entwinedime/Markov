"""Workspace state at the boundaries of a native profiling window."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.environment import RuntimeEnvironment


class WorkspaceCapture:
    def __init__(self, directory: Path, rank: int) -> None:
        self.path = directory / f"rank-{rank}-pid-{os.getpid()}.json"
        self.state: dict[str, Any] = {
            "rank": rank,
            "pid": os.getpid(),
            "memory_caching_environment": os.environ.get("PYTORCH_NO_NPU_MEMORY_CACHING"),
            "probe_installation": RuntimeEnvironment().describe()["workspace_probe"],
            "initial": None,
            "final": None,
        }

    def begin(self) -> None:
        self.state["initial"] = self._snapshot()
        self._write()

    def end(self) -> None:
        self.state["final"] = self._snapshot()
        self._write()

    def _snapshot(self) -> list[dict[str, int]]:
        import torch_npu

        # Workspace snapshot entries require history recording. Preserve an
        # existing configuration; otherwise enable only state, outside profiling.
        history_enabled = torch_npu._C._npu_isHistoryEnabled()
        if not history_enabled:
            torch_npu.npu.memory._record_memory_history(enabled="state", context=None, max_entries=1)

        try:
            snapshot = torch_npu.npu.memory._snapshot()
            return [
                {
                    "device": device,
                    "stream_handle": entry["stream"],
                    "capacity_bytes": entry["size"],
                    "address": entry["addr"],
                }
                for device, trace in enumerate(snapshot["device_traces"])
                for entry in trace
                if entry["action"] == "workspace_snapshot"
            ]
        finally:
            if not history_enabled:
                torch_npu.npu.memory._record_memory_history(enabled=None)

    def _write(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text(json.dumps(self.state, separators=(",", ":")) + "\n", encoding="utf-8")
