"""One recoverable CPU container with explicit inputs and one output directory."""

from __future__ import annotations

import subprocess
from pathlib import Path

from markov_collection.workflow.container_environment import ContainerEnvironment


class ModelingJobFailed(RuntimeError):
    """A recorded modeling container reached a nonzero terminal exit."""


class ModelingJob:
    def __init__(self, environment: ContainerEnvironment, name: str, directory: Path, log: Path) -> None:
        self.environment = environment
        self.name = name
        self.directory = directory
        self.log = log

    def running(self) -> bool:
        state = self.environment.container_state(self.name)
        return state is not None and state["Running"]

    def run(self, arguments: list[str], inputs: set[Path], outputs: set[Path] | None = None) -> None:
        command = [
            "docker",
            "run",
            "--detach",
            "--name",
            self.name,
            "--pull=never",
            "--network=none",
            "--read-only",
            "--cap-drop=ALL",
            "-e",
            "PYTHONDONTWRITEBYTECODE=1",
            "-e",
            "PYTHONPATH=/workspace/trace-sim/src/collection",
        ]
        for path in sorted(inputs):
            command += ["--mount", f"type=bind,src={path},dst={self.environment.container_path(path)},readonly"]
        for path in sorted({self.directory, *(outputs or set())}):
            path.mkdir(parents=True, exist_ok=True)
            command += ["--mount", f"type=bind,src={path},dst={self.environment.container_path(path)}"]
        command += [self.environment.service_image("modeling"), *arguments]

        # A lost host does not terminate this job. The named container and its
        # published output determine recovery; an observation timeout is not exit.
        # Docker retains the complete log across host recovery. Publish one
        # snapshot, rather than appending the same history on every resume.
        self.log.parent.mkdir(parents=True, exist_ok=True)
        with self.log.open("w", encoding="utf-8") as log:
            if self.environment.container_state(self.name) is None:
                subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
            subprocess.run(["docker", "wait", self.name], check=True, stdout=log, stderr=subprocess.STDOUT)
            subprocess.run(["docker", "logs", self.name], check=True, stdout=log, stderr=subprocess.STDOUT)
            state = self.environment.container_state(self.name)
            if state["ExitCode"] != 0:
                raise ModelingJobFailed(f"modeling container exited {state['ExitCode']}; see {self.log}")
