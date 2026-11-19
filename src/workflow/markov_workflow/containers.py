"""Run bounded modeling commands and clean up only their owned containers."""

import math
import subprocess
import time
from uuid import uuid4


class OwnedContainers:
    def __init__(self, names) -> None:
        self.names = set(names)

    def remaining(self) -> set[str]:
        names = subprocess.check_output(["docker", "ps", "--all", "--format", "{{.Names}}"], text=True)
        return self.names.intersection(names.splitlines())

    def close(self) -> None:
        remaining = self.remaining()
        if not remaining:
            return
        result = subprocess.run(["docker", "rm", "--force", *sorted(remaining)], capture_output=True, text=True)
        if result.returncode:

            # An --rm container may disappear between listing and removal; daemon failures must remain failures.
            remaining = self.remaining()
            if remaining:
                raise RuntimeError(f"failed to remove owned containers {sorted(remaining)}: {result.stderr.strip()}")


class ModelingCommand:
    def __init__(self, binary, image, operation, output, *inputs):
        self.name = f"markov-model-{uuid4().hex}"
        self.binary, self.image = binary.resolve(strict=True), image
        self.operation = operation
        self.output, self.inputs = output.parent.resolve() / output.name, inputs

    def arguments(self, allowance):
        command = ["docker", "run", "--rm", "--init", "--runtime", "runc", "--network", "none", "--name", self.name,
                   "--entrypoint", "/usr/bin/timeout", "--mount", f"type=bind,src={self.binary},dst=/app/markov,readonly",
                   "--mount", f"type=bind,src={self.output.parent},dst=/output"]
        paths = []
        for index, source in enumerate(self.inputs):
            path = f"/inputs/{index}.json"
            paths.append(path)
            command.extend(("--mount", f"type=bind,src={source.resolve(strict=True)},dst={path},readonly"))

        command.extend((self.image, "--signal=KILL", f"{allowance}s", "/app/markov", self.operation, *paths,
                        f"/output/{self.output.name}"))
        return command


class ModelingProcess:
    def __init__(self, binary, image, allowance):
        if not math.isfinite(allowance) or allowance <= 0:
            raise ValueError("modeling needs a positive finite allowance")
        self.binary = binary.resolve(strict=True)
        self.image = subprocess.check_output(["docker", "image", "inspect", "--format", "{{.Id}}", image], text=True).strip()
        self.deadline = time.monotonic() + allowance

    def run_model(self, operation, output, *inputs):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("modeling allowance exhausted")
        command = ModelingCommand(self.binary, self.image, operation, output, *inputs)
        try:
            subprocess.run(command.arguments(remaining), check=True, timeout=remaining)
        finally:
            OwnedContainers([command.name]).close()
