"""One native source process, private target workers and a local batch stream."""

from __future__ import annotations

from collections.abc import Iterator
import json
import os
from pathlib import Path
import signal
import subprocess
from typing import Any

from markov_internal.common.paths import ROOT_DIR
from markov_internal.common.process import stop_process
from markov_internal.modeling.backend import build_trace_graph_command
from markov_internal.modeling.run_config import ModelingRunConfig


class SourceSession:
    def __init__(self, config: ModelingRunConfig, directory: Path) -> None:
        self.config = config
        self.directory = directory
        self.log_path = directory / "source_preparation.log"
        self.process: subprocess.Popen[str] | None = None
        self.log = None
        self.prepared = False
        self.failed = False
        self.memory_budget: dict[str, int] = {}

    def execute(self, plan: Path) -> Iterator[dict[str, Any]]:
        if self.process is None:
            self.directory.mkdir(parents=True, exist_ok=True)
            (self.directory / "source_replay_report.json").unlink(missing_ok=True)
            self.log = self.log_path.open("w", encoding="utf-8")
            self.process = subprocess.Popen(
                [*build_trace_graph_command(self.config, target_plan=plan), "--target-session"],
                cwd=ROOT_DIR,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=self.log,
                text=True,
                start_new_session=True,
            )
        else:
            if self.process.poll() is not None:
                raise RuntimeError("native source session ended; open a new prediction session")
            self.process.stdin.write(json.dumps(str(plan)) + "\n")
            self.process.stdin.flush()
            if self.prepared:
                yield {"event": "source_reused"}

        for line in self.process.stdout:
            event = json.loads(line)
            if event.get("event") == "source_prepared":
                self.prepared = True
            if event.get("event") == "batch_completed":
                self.failed |= event["return_code"] != 0
                yield event
                return
            yield event

        self.failed = True
        raise RuntimeError(f"native source session ended before batch completion: {self.process.wait()}")

    def close(self) -> None:
        if self.process is not None:
            try:
                self.process.stdin.close()
                self.process.wait(timeout=3)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.failed = True
            finally:
                stop_process(self.process, timeout_sec=2)
                # A failed native parent may have left workers in its owned group.
                try:
                    os.killpg(self.process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.process.stdout.close()
                self.process = None
                self.prepared = False
        if self.log is not None:
            self.log.close()
            self.log = None
