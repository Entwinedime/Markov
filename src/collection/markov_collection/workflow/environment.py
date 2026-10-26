"""Own the Docker resources of one exploration; prediction mounts only declared inputs."""

from __future__ import annotations

import os
import shlex
import subprocess
from pathlib import Path


class DockerEnvironment:
    def __init__(self, root: Path, run: Path, service: str, devices: list[int]) -> None:
        self.root = root
        self.build = root / "build/automatic-graph"
        self.observer_build = run / "work/observer-build"
        self.label = f"markov.run={run}"
        self.service = service
        self.devices = devices
        self.native: str | None = None
        self.compose = ["docker", "compose", "-f", str(root / "docker/compose/inference.yml")]
        prefix = os.environ.get("MARKOV_IMAGE_PREFIX", "markov-next")
        self.modeling_image = f"{prefix}-modeling:ubuntu24.04"

    def cleanup(self) -> None:
        identities = subprocess.check_output(
            ["docker", "ps", "-aq", "--filter", f"label={self.label}"], text=True, timeout=30
        ).split()
        if identities:
            subprocess.run(["docker", "rm", "-f", *identities], check=True, stdout=subprocess.DEVNULL, timeout=30)
        self.native = None

    def start(self) -> None:
        environment = dict(os.environ, ASCEND_RT_VISIBLE_DEVICES=",".join(map(str, self.devices)))
        command = self.compose + ["run", "--no-deps", "-d", "--label", self.label, self.service, "sleep", "infinity"]
        self.native = subprocess.check_output(command, env=environment, text=True, timeout=30).strip()

    def native_path(self, path: Path) -> str:
        return str(Path("/workspace/trace-sim") / path.relative_to(self.root))

    @staticmethod
    def limited(arguments: list[str], remaining_ns: int) -> list[str]:

        # A lost Docker client must not leave the container's worker running past its allocation.
        seconds = max(1, remaining_ns // 1_000_000_000 - 2)
        return ["timeout", "--kill-after=1s", str(seconds), *arguments]

    def native_command(self, arguments: list[str], output: Path, library: str, remaining_ns: int) -> list[str]:
        return [
            "docker",
            "exec",
            "--workdir",
            self.native_path(output),
            "-e",
            f"LD_PRELOAD={self.native_path(self.observer_build / library)}",
            "-e",
            f"MARKOV_NATIVE_RUNTIME_DIR={self.native_path(output / 'runtime')}",
            "-e",
            f"TRITON_CACHE_DIR={self.native_path(self.observer_build.parent / 'triton-cache')}",
            str(self.native),
            *self.limited(arguments, remaining_ns),
        ]

    def build_command(
        self, directory: Path, definitions: list[str], targets: list[str], remaining_ns: int
    ) -> list[str]:
        directory.parent.mkdir(parents=True, exist_ok=True)
        build = self.native_path(directory)
        configure = ["cmake", "-S", ".", "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", *definitions]
        compile = ["cmake", "--build", build, "--target", *targets, "-j2"]
        script = shlex.join(configure) + " && " + shlex.join(compile)
        return self.compose + [
            "run",
            "--rm",
            "--no-deps",
            "--label",
            self.label,
            "modeling",
            *self.limited(["flock", build + ".lock", "bash", "-c", script], remaining_ns),
        ]

    def tool_command(self, arguments: list[str], inputs: list[Path], output: Path, remaining_ns: int) -> list[str]:
        binary = self.build / "markov"

        # Docker otherwise creates directories for missing file bind sources.
        for source in (binary, *inputs):
            if not source.is_file():
                raise FileNotFoundError(f"tool input is not a file: {source}")

        command = [
            "docker",
            "run",
            "--rm",
            "--init",
            "--network",
            "none",
            "--label",
            self.label,
            "-v",
            f"{binary}:/tool/markov:ro",
            "-v",
            f"{self.build}.lock:/tool/build.lock:ro",
        ]
        for source in inputs:
            command += ["-v", f"{source}:{source}:ro"]
        output.parent.mkdir(parents=True, exist_ok=True)
        command += [
            "-v",
            f"{output.parent}:{output.parent}",
            self.modeling_image,
            *self.limited(
                ["flock", "--shared", "--no-fork", "/tool/build.lock", "/tool/markov", *arguments], remaining_ns
            ),
        ]

        # Protect bind setup here; the worker also holds the same lock through its own lifetime.
        return ["flock", "--shared", "--no-fork", str(self.build) + ".lock", *command]
