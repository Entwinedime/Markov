"""Produce representative templates from a closed native capture."""

import fcntl
import filecmp
import json
import shutil
import subprocess
import time
from uuid import uuid4

from markov_collection.artifacts import write_json
from markov_collection.adapters.runtimes.ascend.export import AscendExport
from markov_workflow.containers import OwnedContainers
from markov_workflow.limits import OperationLimit


class TemplateProduction:
    def __init__(self, arguments, limit=None) -> None:
        self.arguments = arguments
        self.operation_limit = limit if limit is not None else OperationLimit()
        self.output = arguments.output.resolve()
        self.capture = arguments.capture.resolve(strict=True)
        self.binary = arguments.binary.resolve(strict=True)
        if self.output == self.capture or self.capture in self.output.parents:
            raise ValueError("template production must not write inside its immutable native capture")
        self.limit_ns = int(arguments.budget_seconds * 1_000_000_000)
        if self.limit_ns <= 0:
            raise ValueError("postprocessing budget must be positive")

        capture = json.loads((self.capture / "capture.json").read_text())
        self.ranks = [rank for instance in capture["instances"] for rank in instance["observed_roles"]]
        if not self.ranks or len(set(self.ranks)) != len(self.ranks):
            raise ValueError("published capture must contain unique observed workers")
        images = {}
        for role, image in (("source", arguments.source_image), ("modeling", arguments.modeling_image)):
            images[role] = subprocess.check_output(["docker", "image", "inspect", image, "--format", "{{.Id}}"], text=True).strip()
        self.inputs = {"capture_path": str(self.capture), "capture": capture, "images": images,
                       "keep_intermediates": arguments.keep_intermediates}

    def publish_state(self) -> None:
        write_json(self.output / "production.json", self.state, indent=2)

    def discard(self, output: str) -> None:
        path = self.output / output
        if path.is_dir():
            shutil.rmtree(path)
        else:
            path.unlink(missing_ok=True)

    def prepare(self) -> None:
        state = self.output / "production.json"
        executable = self.output / "inputs/markov"
        if state.exists():
            self.state = json.loads(state.read_text())
            if self.state["inputs"] != self.inputs or not filecmp.cmp(self.binary, executable, shallow=False):
                raise ValueError("production inputs changed; create a new output instead of resuming different assets")
            if self.state["running"] is not None:

                # A killed client may leave its container alive; recover only the recorded invocation.
                running = self.state["running"]
                OwnedContainers([running["container"]]).close()
                self.discard(running["output"])
                self.state["running"] = None
                self.state["error"] = "interrupted operation charged its reserved remaining budget"
                self.publish_state()
            return

        if any(path.name != ".lock" for path in self.output.iterdir()):
            raise ValueError("new template production requires an empty directory")

        for name in ("inputs", "work", "logs"):
            (self.output / name).mkdir()
        shutil.copyfile(self.binary, executable)
        executable.chmod(0o500)
        write_json(self.output / "inputs/templates.json", [f"/production/work/{rank}-templates.json" for rank in self.ranks])
        self.state = {"inputs": self.inputs, "spent_ns": 0, "attempts": 0,
                      "completed": [], "running": None, "error": None}
        self.publish_state()

    def step(self, name: str, output: str, image: str, command: list[str]) -> None:
        if name in self.state["completed"]:
            if not (self.output / output).exists():
                raise FileNotFoundError(f"completed production step lost its output: {output}")
            return
        remaining_ns = self.limit_ns - self.state["spent_ns"]
        if remaining_ns <= 0:
            self.state["error"] = "cumulative postprocessing budget exhausted; completed steps remain reusable"
            self.publish_state()
            raise RuntimeError(self.state["error"])
        self.operation_limit.check()
        container = f"markov-produce-{uuid4().hex}"
        self.state["attempts"] += 1
        log = self.output / "logs" / f"{self.state['attempts']:03d}-{name}.log"

        # Reserve before starting: hard interruption cannot reset the cumulative allowance on resume.
        previous_ns = self.state["spent_ns"]
        self.state["spent_ns"] = self.limit_ns
        self.state["running"] = {"container": container, "step": name, "output": output}
        self.state["error"] = None
        self.publish_state()
        started_ns = time.monotonic_ns()
        try:
            with log.open("x") as stream:

                # The cumulative reservation also bounds the container after coordinator interruption.
                command = [
                    "docker", "run", "--rm", "--init", "--network", "none", "--name", container,
                    "--entrypoint", "/usr/bin/timeout", "-e", "LD_PRELOAD=",
                    "--mount", f"type=bind,src={self.capture},dst=/source,readonly",
                    "--mount", f"type=bind,src={self.output},dst=/production",
                    self.inputs["images"][image], "--signal=KILL", f"{remaining_ns / 1_000_000_000}s", *command,
                ]
                code = self.operation_limit.run(command, stream, remaining_ns / 1_000_000_000)
                if code:
                    raise subprocess.CalledProcessError(code, command)
            if not (self.output / output).exists():
                raise FileNotFoundError(f"production command did not publish {output}")
            self.state["completed"].append(name)
        except BaseException as error:
            self.state["error"] = f"{name}: {type(error).__name__}: {error}"
            raise
        finally:
            OwnedContainers([container]).close()
            if name not in self.state["completed"]:
                self.discard(output)
            self.state["spent_ns"] = previous_ns + time.monotonic_ns() - started_ns
            self.state["running"] = None
            self.publish_state()

    def run(self) -> None:
        self.output.mkdir(parents=True, exist_ok=True)
        with (self.output / ".lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.prepare()
            pending = [] if "link-templates" in self.state["completed"] else self.ranks
            for rank in pending:
                facts, graph, template = (f"work/{rank}-facts.json", f"work/{rank}-graph.json", f"work/{rank}-templates.json")
                exported = f"work/{rank}-sdk"
                if f"{rank}-templates" not in self.state["completed"]:
                    self.step(f"{rank}-export", exported, "source", AscendExport.command(rank, f"/production/{exported}"))
                    self.step(f"{rank}-interpret", facts, "source", [
                        "python3", "-m", "markov_collection", "interpret", "--input", "/source", "--rank", str(rank),
                        "--trace", f"/production/{exported}/trace.json", "--output", f"/production/{facts}",
                        *(["--evidence"] if self.arguments.keep_intermediates else []),
                    ])
                    self.step(f"{rank}-project", graph, "modeling", [
                        "/production/inputs/markov", "project", f"/production/{facts}", f"/production/{graph}",
                        *(["--evidence"] if self.arguments.keep_intermediates else []),
                    ])
                self.step(f"{rank}-templates", template, "modeling", [
                    "/production/inputs/markov", "templates", f"/production/{graph}", f"/production/{template}",
                ])

            # One captured set supplies local variants; linking never searches other runs or changes worker domains.
            self.step("link-templates", "assets", "modeling", [
                "/production/inputs/markov", "link-templates", "/production/inputs/templates.json", "/production/assets",
            ])
            if not self.arguments.keep_intermediates:
                for rank in self.ranks:
                    for suffix in ("sdk", "facts.json", "graph.json", "templates.json"):
                        self.discard(f"work/{rank}-{suffix}")

            # Consumers receive an index only after every representative library has been published.
            write_json(self.output / "result.json", {
                "templates": [f"assets/{rank}.json" for rank in self.ranks],
                "observed_roles": self.ranks, "postprocessing_ns": self.state["spent_ns"],
                "qualification": "compiled_source_templates",
            }, indent=2)
        print(f"Created {self.output / 'result.json'}")
