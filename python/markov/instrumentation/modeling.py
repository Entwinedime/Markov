"""Own isolated, device-free modeling subprocesses for prediction and diagnostics."""

import os
from pathlib import Path
import subprocess

from markov.artifacts.request import read_json
from markov.artifacts.writing import write_json
from markov.instrumentation.containers import container_state


class ModelingProcess:
    """Own one isolated modeling command and its explicitly selected read-only assets."""

    def __init__(self, output: Path, memory_limit_gib: int, executable_name: str = "markov_core"):
        self.output = output
        self.memory_limit_gib = memory_limit_gib
        self.cid = output / "container.id"
        self.report = output / "report.json"
        self.executable_name = executable_name

    def check_available(self) -> None:
        current = container_state(self.cid)
        if current is not None and current["Running"]:
            raise ValueError("This modeling output still has a running container")
        self.report.with_name(self.report.name + ".partial").unlink(missing_ok=True)

    def run(self, command: str, prepared: Path, assets: tuple[Path, ...]) -> dict:
        executable = Path(__file__).resolve().parents[3] / "build/modeling/Release" / self.executable_name
        container_executable = f"/opt/markov/{self.executable_name}"
        self.cid.unlink(missing_ok=True)
        args = [
            "docker",
            "run",
            "--rm",
            "--init",
            "--network",
            "none",
            "--pids-limit",
            "256",
            "--cidfile",
            str(self.cid),
            "--memory",
            f"{self.memory_limit_gib}g",
            "--memory-swap",
            f"{self.memory_limit_gib}g",
        ]

        # Mount only explicit assets. Prediction callers pass no evaluator inputs
        # or repository/data root, including unused neighboring observations.
        for source, target, writable in (
            (executable, container_executable, False),
            *[(asset, str(asset), False) for asset in assets],
            (prepared, str(prepared), False),
            (self.output, str(self.output), True),
        ):
            args += ["--mount", f"type=bind,source={source},target={target}" + ("" if writable else ",readonly")]
        args += [
            os.environ.get("MARKOV_MODELING_IMAGE", "markov-modeling:ubuntu24.04"),
            container_executable,
            command,
            str(prepared),
            str(self.report),
        ]
        write_json(self.output / "invocation.json", args)
        with (self.output / "run.log").open("w", encoding="utf-8") as log:
            process = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
            try:
                code = process.wait()
            finally:
                if process.poll() is None:
                    if self.cid.exists():
                        subprocess.run(
                            ["docker", "rm", "-f", self.cid.read_text().strip()], stdout=log, stderr=log, timeout=20
                        )
                    process.terminate()
                    process.wait(timeout=20)
        if code:
            result = {"status": "failed", "exit_code": code, "log": str(self.output / "run.log")}
            write_json(self.report, result)
            return result
        return read_json(self.report)
