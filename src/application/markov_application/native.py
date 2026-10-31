"""Run one native-controlled prediction in isolated, device-free containers."""

import argparse
import math
import os
import signal
import subprocess
import sys
import time
from contextlib import ExitStack
from pathlib import Path
from tempfile import TemporaryDirectory
from uuid import uuid4

from markov_application.command import CommandFailed, terminate


class NativeExecution:
    def __init__(self, root: Path, output: Path, timeout_seconds: float, task_directory: Path | None = None) -> None:
        self.binary = root / "build/automatic-graph/markov"
        self.lock = self.binary.parent.with_suffix(".lock")
        self.output = output.resolve()
        self.logs = self.output.with_suffix(".logs")
        self.timeout_seconds = timeout_seconds
        self.label = f"markov.control={uuid4().hex}"
        self.task_directory = task_directory
        self.prefix = os.environ.get("MARKOV_IMAGE_PREFIX", "markov-next")

    def run(self, templates: Path, workload: Path, details: bool) -> None:
        templates, workload = (path.resolve(strict=True) for path in (templates, workload))
        logs = [self.logs / (role + ".log") for role in ("core", "native")]
        outputs = {path.resolve() for path in (self.output, *logs)}

        # Log streams truncate in place, including through a hard link to an input.
        for path in (self.binary, self.lock, templates, workload):
            if not path.is_file():
                raise FileNotFoundError(f"Prediction input is not a file: {path}")
            if path.resolve() in outputs or any(log.exists() and log.samefile(path) for log in logs):
                raise ValueError("Prediction output and logs must not overwrite an input")
        self.output.parent.mkdir(parents=True, exist_ok=True)
        self.logs.mkdir(exist_ok=True)
        staging = TemporaryDirectory(prefix=f".{self.output.name}.", dir=self.output.parent)
        pending = Path(staging.name) / self.output.name

        core = [
            "flock",
            "--shared",
            "--no-fork",
            str(self.lock),
            *self.container(
                "modeling",
                "ubuntu24.04",
                {
                    self.binary: ("/tool/markov", "ro"),
                    self.lock: ("/tool/build.lock", "ro"),
                    templates: ("/inputs/templates.json", "ro"),
                    workload: ("/inputs/serving.json", "ro"),
                    pending.parent: ("/output", "rw"),
                },
            ),
            "flock",
            "--shared",
            "--no-fork",
            "/tool/build.lock",
            "/tool/markov",
            "simulate",
            "--templates",
            "/inputs/templates.json",
            "--input",
            "/inputs/serving.json",
            "--output",
            f"/output/{pending.name}",
            "--control",
            "native",
        ]
        if details:
            core.append("--details")
        native = [
            *self.container("control", "ubuntu22.04", {}),
            "python3",
            "-m",
            "markov_control",
            "--kernel-python",
            "/opt/markov/sglang/sgl-kernel/python",
        ]
        try:
            self.connect(core, native)

            # Native participants must exit successfully before a completed core graph becomes a result.
            pending.replace(self.output)
        finally:
            staging.cleanup()
        print(f"Prediction saved to {self.output}; logs: {self.logs}")

    def container(self, role: str, version: str, mounts: dict[Path, tuple[str, str]]) -> list[str]:
        command = [
            "docker",
            "run",
            "--rm",
            "--init",
            "-i",
            "--network",
            "none",
            "--label",
            self.label,
            "--entrypoint",
            "timeout",
        ]
        if self.task_directory is not None:

            # The task can recover both workers even if its prediction launcher was lost.
            command += ["--label", f"markov.run={self.task_directory}"]
        for source, (destination, access) in mounts.items():
            command += ["-v", f"{source}:{destination}:{access}"]
        return [
            *command,
            f"{self.prefix}-{role}:{version}",
            "--kill-after=1s",
            str(math.ceil(self.timeout_seconds)),
        ]

    def connect(self, core: list[str], native: list[str]) -> None:
        processes = []
        previous_signal = signal.signal(signal.SIGTERM, self.interrupt)
        deadline = time.monotonic() + self.timeout_seconds
        try:
            with ExitStack() as resources:
                core_log = resources.enter_context((self.logs / "core.log").open("w"))
                native_log = resources.enter_context((self.logs / "native.log").open("w"))

                # Pipe ownership leaves the parent as soon as both children have their endpoints.
                with ExitStack() as pipes:
                    core_read, native_write = (
                        pipes.enter_context(os.fdopen(fd, mode))
                        for fd, mode in zip(os.pipe(), ("rb", "wb"), strict=True)
                    )
                    native_read, core_write = (
                        pipes.enter_context(os.fdopen(fd, mode))
                        for fd, mode in zip(os.pipe(), ("rb", "wb"), strict=True)
                    )
                    processes.append(subprocess.Popen(core, stdin=core_read, stdout=core_write, stderr=core_log))
                    processes.append(
                        subprocess.Popen(native, stdin=native_read, stdout=native_write, stderr=native_log)
                    )

                for role, process in zip(("core", "native"), processes, strict=True):
                    code = process.wait(timeout=max(0, deadline - time.monotonic()))
                    if code:

                        # Core EOF can follow a native failure; preserve both causes through the task's excerpt.
                        sys.stderr.write(CommandFailed.context(self.logs / "core.log") + "\n")
                        native_context = CommandFailed.context(self.logs / "native.log")
                        raise RuntimeError(
                            f"{role} exited with status {code}; logs: {self.logs}\nnative: {native_context}"
                        )
        finally:
            try:
                for process in processes:
                    terminate(process)
            finally:
                try:

                    # Stop launchers before the final Docker snapshot so they cannot create another worker.
                    self.cleanup()
                finally:
                    signal.signal(signal.SIGTERM, previous_signal)

    def cleanup(self) -> None:
        identities = subprocess.check_output(
            ["docker", "ps", "-aq", "--filter", f"label={self.label}"], text=True, timeout=30
        ).split()
        if identities:
            subprocess.run(["docker", "rm", "-f", *identities], check=True, stdout=subprocess.DEVNULL, timeout=30)

    @staticmethod
    def interrupt(signum, frame) -> None:
        raise SystemExit(128 + signum)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--templates", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=float, default=300)
    parser.add_argument("--task-directory", type=Path, help="associate prediction workers with their owning task")
    parser.add_argument("--details", action="store_true")
    arguments = parser.parse_args()
    if arguments.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be positive")

    root = Path(__file__).resolve().parents[3]
    NativeExecution(root, arguments.output, arguments.timeout_seconds, arguments.task_directory).run(
        arguments.templates, arguments.input, arguments.details
    )


if __name__ == "__main__":
    main()
