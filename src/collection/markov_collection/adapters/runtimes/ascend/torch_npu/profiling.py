"""Ascend profiling configuration and allocator window boundaries."""

import os
import threading
import time
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from contextvars import ContextVar
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.torch_npu.workspace import WorkspaceCapture
from markov_collection.artifacts.json_file import write_json


class AscendProfileWindow:
    configuration_hook = "torch_npu.profiler.experimental_config._ExperimentalConfig.__init__"

    def __init__(self, directory: Path) -> None:
        self.directory = directory
        self._workspace: WorkspaceCapture | None = None
        self._controls: list[dict[str, Any]] = []
        self._starting = ContextVar("markov_ascend_starting_profile", default=False)

    def begin(self, rank: int) -> None:
        self._rank = rank
        self._workspace = WorkspaceCapture(self.directory, rank)
        self._workspace.begin()

    @contextmanager
    def starting(self) -> Iterator[None]:
        token = self._starting.set(True)
        try:
            with self._control("start"):
                yield
        finally:
            self._starting.reset(token)

    @contextmanager
    def stopping(self) -> Iterator[None]:
        with self._control("stop"):
            yield

    @contextmanager
    def _control(self, operation: str) -> Iterator[None]:
        # These are runtime lifecycle observations, not serving blocks. The
        # native profiler may not export a marker enclosing its own stop call.
        record = {
            "operation": operation,
            "rank": self._rank,
            "pid": os.getpid(),
            "tid": threading.get_native_id(),
            "start_ns": time.perf_counter_ns(),
            "completed": False,
        }
        try:
            yield
            record["completed"] = True
        finally:
            record["end_ns"] = time.perf_counter_ns()
            self._controls.append(record)
            directory = self.directory.parent / "profile_controls"
            directory.mkdir(parents=True, exist_ok=True)
            write_json(directory / f"rank-{self._rank}-pid-{os.getpid()}.json", self._controls)

    def configure(self, original: Callable[..., Any], config: Any, *args: Any, **kwargs: Any) -> Any:
        if self._starting.get():
            from torch_npu.profiler import ProfilerLevel

            # Level2 exposes runtime copy waits absent from the default Level1 trace.
            kwargs["profiler_level"] = ProfilerLevel.Level2
            # Keep published exports and clock/device metadata, but let the vendor
            # remove duplicate analysis intermediates after export has completed.
            kwargs["data_simplification"] = True

        return original(config, *args, **kwargs)

    def end(self) -> None:
        if self._workspace is not None:
            self._workspace.end()
