"""Own one prediction process and its isolated core/control lifecycle."""

import json
import math
import os
import subprocess
import time
from contextlib import ExitStack
from uuid import uuid4

from markov_workflow.adapters.sglang import SourceDomain
from markov_workflow.containers import OwnedContainers
from markov_workflow.source_productions import SourceProductions


class PredictionProcess:
    def __init__(self, arguments) -> None:
        self.arguments = arguments

    def run(self) -> None:
        args = self.arguments
        if not math.isfinite(args.timeout) or args.timeout <= 0:
            raise ValueError("Prediction timeout must be positive")
        deadline = time.monotonic() + args.timeout
        source = json.loads(args.input.read_text())
        templates, primary, document = SourceProductions(args.production).prepare(source, modeled=args.costs is not None)
        timing_asset = args.costs or args.execution_inputs
        timing_mount, timing_option = [], []
        if timing_asset is not None:
            option = "--costs" if args.costs is not None else "--execution-inputs"
            path = f"/inputs/{option[2:]}.json"
            timing_mount = ["--mount", f"type=bind,src={timing_asset.resolve(strict=True)},dst={path},readonly"]
            timing_option = [option, path]
        binary = args.binary.resolve(strict=True)
        descriptions = {
            role: json.loads(subprocess.check_output(
                ["docker", "image", "inspect", image], text=True, timeout=max(0, deadline - time.monotonic()),
            ))[0]
            for role, image in (("modeling", args.modeling_image), ("control", args.control_image))
        }
        SourceDomain.require_control(descriptions["control"], primary)
        images = {role: image["Id"] for role, image in descriptions.items()}
        output = args.output.resolve()
        output.mkdir(parents=True)
        task = output / ".task.json"
        stem = f"markov-serve-{uuid4().hex}"
        names = [f"{stem}-core", f"{stem}-control"]
        try:
            with task.open("x") as stream:
                json.dump(document, stream)
                stream.write("\n")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Prediction preparation exhausted its time allowance")

            # The core alone receives the selected timing asset.
            commands = [
                [
                    "docker", "run", "--rm", "--init", "-i", "--runtime", "runc", "--network", "none", "--name", names[0],
                    "--entrypoint", "/usr/bin/timeout",
                    "--mount", f"type=bind,src={binary},dst=/app/markov,readonly",
                    *[item for index, path in enumerate(templates) for item in ("--mount", f"type=bind,src={path},dst=/inputs/templates/{index}.json,readonly")],
                    "--mount", f"type=bind,src={task},dst=/inputs/task.json,readonly",
                    *timing_mount,
                    "--mount", f"type=bind,src={output},dst=/output",
                    images["modeling"], "--signal=KILL", f"{remaining}s",
                    "/app/markov", "serve", "/inputs/templates", "/inputs/task.json", "/output/.result.json",
                    *(["--trace", "/output/execution.partial.jsonl"] if args.execution_trace else []),
                    *timing_option,
                ],
                [
                    "docker", "run", "--rm", "--init", "-i", "--runtime", "runc", "--network", "none", "--shm-size", "512m", "--name", names[1],
                    "--entrypoint", "/usr/bin/timeout", images["control"], "--signal=KILL", f"{remaining}s",
                    "python3", "-m", "markov_control", "--kernel-python", "/opt/markov/sglang/sgl-kernel/python",
                ],
            ]
            self.execute(commands, names, output, deadline)

            # Both endpoints and their owned containers must close before the result becomes consumable.
            if args.execution_trace:
                os.link(output / "execution.partial.jsonl", output / "execution.jsonl")
                (output / "execution.partial.jsonl").unlink()
            os.link(output / ".result.json", output / "result.json")
        finally:
            (output / ".result.json").unlink(missing_ok=True)
            task.unlink(missing_ok=True)
        print(f"Created {output / 'result.json'}")

    def execute(self, commands, names, output, deadline) -> None:
        processes = []
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
