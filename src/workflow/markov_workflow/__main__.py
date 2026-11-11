"""Produce template assets or run a prediction in isolated containers."""

import argparse
import json
import math
import os
import subprocess
import time
from contextlib import ExitStack
from pathlib import Path
from uuid import uuid4

from markov_workflow.adapters.sglang import SGLangTask
from markov_workflow.containers import OwnedContainers


class ServingProcess:
    def __init__(self, arguments) -> None:
        self.arguments = arguments

    def run(self) -> None:
        args = self.arguments
        templates = []
        productions = []
        for directory in [args.production, *args.local_production]:
            directory = directory.resolve(strict=True)
            publication = json.loads((directory / "result.json").read_text())
            libraries = []
            for name in publication["templates"]:
                path = (directory / name).resolve(strict=True)
                if not path.is_relative_to(directory):
                    raise ValueError("template publication refers outside its production directory")
                templates.append(path)
                libraries.append(json.loads(path.read_text()))
            productions.append(libraries)
        source = json.loads(args.input.read_text())
        document = SGLangTask(productions[0], productions[1:]).document(source)
        if args.execution_inputs is not None:
            document["execution_inputs"] = json.loads(args.execution_inputs.read_text())
        binary = args.binary.resolve(strict=True)
        if not math.isfinite(args.timeout) or args.timeout <= 0:
            raise ValueError("Prediction timeout must be positive")
        images = {role: subprocess.check_output(["docker", "image", "inspect", image, "--format", "{{.Id}}"], text=True).strip()
                  for role, image in (("modeling", args.modeling_image), ("control", args.control_image))}
        output = args.output.resolve()
        output.mkdir(parents=True)
        task = output / ".task.json"
        with task.open("x") as stream:
            json.dump(document, stream)
            stream.write("\n")
        stem = f"markov-serve-{uuid4().hex}"
        names = [f"{stem}-core", f"{stem}-control"]

        # Container-local deadlines survive a killed coordinator; logs retain the resolved image and exact command.
        commands = [
            [
                "docker", "run", "--rm", "--init", "-i", "--network", "none", "--name", names[0],
                "--entrypoint", "/usr/bin/timeout",
                "--mount", f"type=bind,src={binary},dst=/app/markov,readonly",
                *[item for index, path in enumerate(templates) for item in ("--mount", f"type=bind,src={path},dst=/inputs/templates/{index}.json,readonly")],
                "--mount", f"type=bind,src={task},dst=/inputs/task.json,readonly",
                "--mount", f"type=bind,src={output},dst=/output",
                images["modeling"], "--signal=KILL", f"{args.timeout}s",
                "/app/markov", "serve", "/inputs/templates", "/inputs/task.json", "/output/.result.json",
                *(["--trace", "/output/execution.partial.jsonl"] if args.execution_trace else []),
            ],
            [
                "docker", "run", "--rm", "--init", "-i", "--network", "none", "--shm-size", "512m", "--name", names[1],
                "--entrypoint", "/usr/bin/timeout", images["control"], "--signal=KILL", f"{args.timeout}s",
                "python3", "-m", "markov_control", "--kernel-python", "/opt/markov/sglang/sgl-kernel/python",
            ],
        ]
        try:
            self.execute(commands, names, output)

            # Both endpoints and their owned containers must close before the result becomes consumable.
            if args.execution_trace:
                os.link(output / "execution.partial.jsonl", output / "execution.jsonl")
                (output / "execution.partial.jsonl").unlink()
            os.link(output / ".result.json", output / "result.json")
        finally:
            (output / ".result.json").unlink(missing_ok=True)
            task.unlink()
        print(f"Created {output / 'result.json'}")

    def execute(self, commands, names, output) -> None:
        processes = []
        deadline = time.monotonic() + self.arguments.timeout
        try:
            with ExitStack() as logs, ExitStack() as pipes:
                core_read, control_write = os.pipe()
                control_read, core_write = os.pipe()
                for descriptor in (core_read, control_write, control_read, core_write):
                    pipes.callback(os.close, descriptor)

                # Direct pipes carry the protocol; the application never parses or rewrites native decisions.
                for command, reader, writer, name in zip(
                    commands, (core_read, control_read), (core_write, control_write), ("core", "control")
                ):
                    log = logs.enter_context((output / f"{name}.log").open("x"))
                    log.write(json.dumps(command) + "\n")
                    log.flush()
                    processes.append(subprocess.Popen(command, stdin=reader, stdout=writer, stderr=log))
                pipes.close()
                for process, name in zip(processes, names):
                    code = process.wait(timeout=max(0, deadline - time.monotonic()))
                    if code:
                        raise RuntimeError(f"{name} exited with {code}; inspect {output}")

        finally:

            # Docker clients can exit before their containers; remove only this invocation's named resources.
            try:
                OwnedContainers(names).close()
            finally:
                for process in processes:
                    if process.poll() is None:
                        process.kill()
                    process.wait()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    planning = commands.add_parser("plan-sources", help="select and publish an initial capture input per source domain")
    planning.add_argument("--input", type=Path, required=True)
    planning.add_argument("--output", type=Path, required=True)
    prediction = commands.add_parser("predict", help="predict from published template productions")
    prediction.add_argument("--production", type=Path, required=True, help="one complete source job supplying target workers")
    prediction.add_argument("--local-production", type=Path, action="append", default=[], help="additional source of compatible local implementations")
    prediction.add_argument("--input", type=Path, required=True)
    prediction.add_argument("--timeout", type=float, required=True)
    prediction.add_argument("--control-image", default="markov-next-control:ubuntu22.04")
    prediction.add_argument("--execution-trace", action="store_true", help="stream optional expanded graphs, dependencies and completion events")
    prediction.add_argument("--execution-inputs", type=Path, help="supply checked per-actor time conditions and native ingress rounds")
    production = commands.add_parser("produce", help="project a closed native capture into graphs or source templates")
    production.add_argument("--product", choices=("graphs", "templates"), default="templates")
    production.add_argument("--capture", type=Path, required=True)
    production.add_argument("--budget-seconds", type=float, required=True, help="cumulative allowance across resumes; may be explicitly raised")
    production.add_argument("--source-image", default="markov-next-sglang:ubuntu22.04")
    production.add_argument("--keep-intermediates", action="store_true")
    capture = commands.add_parser("capture", help="capture or resume a native task with cumulative allowances")
    capture.add_argument("--input", type=Path, required=True)
    capture.add_argument("--deployment", type=Path, required=True)
    capture.add_argument("--output", type=Path, required=True)
    capture.add_argument("--timeout", type=float, required=True)
    capture.add_argument("--budget-seconds", type=float, required=True, help="cumulative native allowance across attempts and resumes")
    capture.add_argument("--attempt-limit", type=int, required=True, help="cumulative native attempts; each invocation starts at most one")
    capture.add_argument("--source-image", default="markov-next-sglang:ubuntu22.04")
    capture.add_argument("--resource-trace", action="store_true", help="record optional resource and input-read evidence")
    acquisition = commands.add_parser("acquire-sources", help="capture and compile a finite plan under shared cumulative allowances")
    acquisition.add_argument("--plan", type=Path, required=True)
    acquisition.add_argument("--observations", type=Path, help="JSON map of planned domain IDs to published capture task paths; native inputs must match")
    acquisition.add_argument("--deployment", type=Path, required=True)
    acquisition.add_argument("--timeout", type=float, required=True, help="maximum seconds per native attempt")
    acquisition.add_argument("--native-budget-seconds", type=float, required=True)
    acquisition.add_argument("--postprocessing-budget-seconds", type=float, required=True)
    acquisition.add_argument("--attempt-limit", type=int, required=True)
    acquisition.add_argument("--storage-bytes", type=int, required=True, help="monitored logical bytes under this collection root")
    acquisition.add_argument("--source-image", default="markov-next-sglang:ubuntu22.04")
    acquisition.add_argument("--keep-intermediates", action="store_true")
    acquisition.add_argument("--resource-trace", action="store_true", help="record optional resource and input-read evidence")
    for command in (production, prediction, acquisition):
        command.add_argument("--output", type=Path, required=True)
        command.add_argument("--binary", type=Path, required=True)
        command.add_argument("--modeling-image", default="markov-next-modeling:ubuntu24.04")
    arguments = parser.parse_args()
    if arguments.command == "plan-sources":
        from markov_workflow.planning import PlanningProcess

        PlanningProcess(arguments).run()
    elif arguments.command == "produce":
        from markov_workflow.production import CaptureProduction

        CaptureProduction(arguments).run()
    elif arguments.command == "capture":
        from markov_workflow.capture import CaptureProcess

        CaptureProcess(arguments).run()
    elif arguments.command == "acquire-sources":
        from markov_workflow.acquisition import SourceAcquisition

        SourceAcquisition(arguments).run()
    else:
        ServingProcess(arguments).run()


if __name__ == "__main__":
    main()
