"""Capture one native job with cumulative accounting and explicit invocation-by-invocation retries."""

import fcntl
import json
import math
import subprocess
import time
from pathlib import Path
from uuid import uuid4

from markov_adapters.sglang.configuration import CaptureInput
from markov_collection.artifacts import write_json
from markov_workflow.adapters.ascend import AscendDeployment
from markov_workflow.containers import OwnedContainers
from markov_workflow.limits import OperationLimit


class CaptureProcess:
    @staticmethod
    def published(directory: Path, job: dict) -> Path:
        """Resolve an explicit completed observation without changing its native provenance."""
        directory = directory.resolve(strict=True)
        state = json.loads((directory / "execution.json").read_text())
        if state["inputs"]["job"] != job or json.loads((directory / "input.json").read_text()) != job:
            raise ValueError(f"published capture differs from the planned native input: {directory}")
        attempt = state["attempts"][-1]
        if attempt["status"] != "exited" or attempt["exit_code"] != 0:
            raise ValueError(f"published capture has no successful terminal attempt: {directory}")
        publication = json.loads((directory / "result.json").read_text())
        source = (directory / publication["source"]).resolve(strict=True)
        if not source.is_relative_to(directory):
            raise ValueError("published source lies outside its native task")
        return source

    def __init__(self, arguments, limit=None) -> None:
        self.arguments = arguments
        self.operation_limit = limit if limit is not None else OperationLimit()
        job = json.loads(arguments.input.read_text())
        serving = CaptureInput(job)
        workers = sum(instance.configuration.tp * instance.configuration.pp for instance in serving.instances)
        deployment = json.loads(arguments.deployment.read_text())
        self.deployment = AscendDeployment(deployment, workers, len(serving.instances) > 1)
        self.options = self.deployment.options()
        self.model = Path(serving.model["path"]).resolve(strict=True)
        destination = str(Path(serving.model["path"]).absolute())
        self.output = arguments.output.resolve()
        if self.output == self.model or self.model in self.output.parents:
            raise ValueError("capture output must not write inside the model directory")
        for value in (arguments.timeout, arguments.budget_seconds):
            if not math.isfinite(value) or value <= 0:
                raise ValueError("native timeout and cumulative budget must be positive finite seconds")
        if arguments.attempt_limit <= 0:
            raise ValueError("native attempt limit must be positive")
        self.limit_ns = int(arguments.budget_seconds * 1_000_000_000)
        self.timeout_ns = int(arguments.timeout * 1_000_000_000)
        if min(self.limit_ns, self.timeout_ns) < 1:
            raise ValueError("native allowances must be at least one nanosecond")
        image = subprocess.check_output([
            "docker", "image", "inspect", arguments.source_image, "--format", "{{.Id}}",
        ], text=True).strip()
        self.inputs = {
            "job": job, "deployment": deployment, "image": image,
            "model_path": str(self.model), "model_destination": destination,
            "resource_trace": arguments.resource_trace,
        }

    def save(self) -> None:
        write_json(self.output / "execution.json", self.state, indent=2)

    def prepare(self) -> None:
        path = self.output / "execution.json"
        if path.exists():
            self.state = json.loads(path.read_text())
            if self.state["inputs"] != self.inputs or json.loads((self.output / "input.json").read_text()) != self.inputs["job"]:
                raise ValueError("capture inputs, deployment or image changed; use a new task directory")
            return
        if any(path.name != ".lock" for path in self.output.iterdir()):
            raise ValueError("new capture task requires an empty directory")
        self.state = {"inputs": self.inputs, "attempts": []}
        write_json(self.output / "input.json", self.inputs["job"], indent=2, replace=False)
        self.save()

    def recover(self, attempt: dict) -> None:
        owned = OwnedContainers([attempt["container"]])
        if attempt["exit_code"] is None and owned.remaining():
            status = json.loads(subprocess.check_output([
                "docker", "inspect", attempt["container"], "--format", "{{json .State}}",
            ], text=True))
            if status["Status"] == "exited":
                attempt["exit_code"] = status["ExitCode"]

        # A retained container supplies exit evidence after client interruption; a raw manifest alone does not.
        owned.close()
        attempt["status"] = "exited" if attempt["exit_code"] == 0 else "failed"
        attempt["error"] = None if attempt["status"] == "exited" else "interrupted native attempt; reserved time remains charged"
        self.save()

    def capture(self, allowance_ns: int) -> None:
        number = len(self.state["attempts"]) + 1
        output = self.output / "attempts" / str(number)
        container = f"markov-capture-{uuid4().hex}"
        command = [
            "docker", "run", "--init", "--name", container, *self.options,
            "--workdir", str(Path.cwd()), "--entrypoint", "/usr/bin/timeout",
            "--mount", f"type=bind,src={self.model},dst={self.inputs['model_destination']},readonly",
            "--mount", f"type=bind,src={self.output / 'input.json'},dst=/input.json,readonly",
            "--mount", f"type=bind,src={output},dst=/capture",
            self.inputs["image"], "--signal=KILL", f"{allowance_ns / 1_000_000_000}s", "/opt/markov/capture.sh",
            "--input", "/input.json", "--output", "/capture/source",
            *(["--resource-trace"] if self.arguments.resource_trace else []),
        ]
        attempt = {
            "number": number, "container": container, "command": command, "timeout_ns": allowance_ns,
            "charged_ns": allowance_ns, "elapsed_ns": None, "exit_code": None, "status": "running", "error": None,
        }

        # Reserve before launch; the container's own deadline also survives a killed coordinator.
        self.state["attempts"].append(attempt)
        self.save()
        started_ns = time.monotonic_ns()
        try:
            output.mkdir(parents=True)
            with (output / "capture.log").open("x") as log:
                attempt["exit_code"] = self.operation_limit.run(command, log, allowance_ns / 1_000_000_000)
            self.save()
            if attempt["exit_code"]:
                raise subprocess.CalledProcessError(attempt["exit_code"], command)
        except BaseException as error:
            attempt["error"] = f"{type(error).__name__}: {error}"
            raise
        finally:
            OwnedContainers([container]).close()
            attempt["elapsed_ns"] = time.monotonic_ns() - started_ns
            attempt["charged_ns"] = attempt["elapsed_ns"]
            attempt["status"] = "exited" if attempt["exit_code"] == 0 else "failed"
            self.save()

    def run(self) -> None:
        self.output.mkdir(parents=True, exist_ok=True)
        with (self.output / ".lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.prepare()
            attempts = self.state["attempts"]
            if attempts and attempts[-1]["status"] == "running":
                self.recover(attempts[-1])
            spent_ns = sum(attempt["charged_ns"] for attempt in attempts)
            if not attempts or attempts[-1]["status"] != "exited":
                if len(attempts) >= self.arguments.attempt_limit or spent_ns >= self.limit_ns:
                    raise RuntimeError("cumulative native time or attempt allowance exhausted")
                self.operation_limit.check()
                self.capture(min(self.timeout_ns, self.limit_ns - spent_ns))

            # A completed attempt is reused even if final publication was interrupted; no second Engine is needed.
            source = self.output / "attempts" / str(attempts[-1]["number"]) / "source"
            capture = json.loads((source / "capture.json").read_text())
            write_json(self.output / "result.json", {
                "source": str(source.relative_to(self.output)), **capture,
                "native_charged_ns": sum(attempt["charged_ns"] for attempt in attempts), "attempts": len(attempts),
            }, indent=2)
        print(f"Created {self.output / 'result.json'}")
