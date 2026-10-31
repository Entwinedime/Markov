"""Run bounded device jobs and recover their actual container before starting another."""

from __future__ import annotations

from datetime import datetime, timezone
import fcntl
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from markov.artifacts.capture import CaptureSettings, DeviceCommand
from markov.artifacts.request import read_json
from markov.artifacts.writing import write_json
from markov.adapters.ascend.capture import AscendDevices
from markov.instrumentation.containers import container_state


class CaptureBudgetExceeded(ValueError):
    pass


class DeviceTask:
    """One cumulative budget and output owner; callers recover before selecting work."""

    def __init__(self, settings: CaptureSettings, directory: Path):
        self.settings = settings
        self.directory = directory
        self.state_path = directory / "capture_state.json"
        self.state = read_json(self.state_path) if self.state_path.exists() else {"attempts": []}
        self.runtime = AscendDevices(settings.devices)

    def retained_bytes(self) -> int:
        def handle_walk_error(error: OSError) -> None:
            if not isinstance(error, FileNotFoundError):
                raise error

        total = 0

        # Exporters retire both files and directories while the budget is checked.
        for directory, _, names in os.walk(self.directory, onerror=handle_walk_error):
            for name in names:
                path = Path(directory) / name
                try:
                    if path.is_file():
                        total += path.stat().st_size
                except FileNotFoundError:
                    continue
        return total

    def remaining(self) -> dict:
        remaining = dict(self.settings.budget)
        remaining["starts"] -= len(self.state["attempts"])
        for attempt in self.state["attempts"]:
            for key in ("wall_sec", "requests", "input_tokens", "output_tokens"):
                remaining[key] -= attempt[key]
        remaining["wall_sec"] -= self.state.get("processing_wall_sec", 0)
        remaining["wall_sec"] = int(remaining["wall_sec"])
        return remaining

    def record_processing(self, wall_sec: float) -> None:
        self.state["processing_wall_sec"] = self.state.get("processing_wall_sec", 0) + wall_sec
        self.directory.mkdir(parents=True, exist_ok=True)
        write_json(self.state_path, self.state)

    @staticmethod
    def remove_container(output: Path) -> None:
        cid = output / "container.id"
        if cid.exists():
            with (output / "run.log").open("a", encoding="utf-8") as log:
                subprocess.run(["docker", "rm", "-f", cid.read_text().strip()], stdout=log, stderr=log, timeout=20)

    def recover(self) -> None:
        for attempt in self.state["attempts"]:
            if attempt["status"] != "running":
                continue
            output = Path(attempt["output"])
            with (Path(tempfile.gettempdir()) / "markov-device-capture.lock").open("a") as lease:
                fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
                state = container_state(output / "container.id")
                while state is not None and state["Running"]:
                    elapsed = time.time() - attempt["started_at"]
                    if (
                        elapsed >= attempt["wall_limit_sec"]
                        or self.retained_bytes() > self.settings.budget["retained_mib"] * 1024**2
                    ):
                        attempt["limit_reached"] = (
                            "wall_sec" if elapsed >= attempt["wall_limit_sec"] else "retained_mib"
                        )
                        self.remove_container(output)
                        state = None
                        break
                    time.sleep(2)
                    state = container_state(output / "container.id")
                if state is not None and state["Status"] == "exited":
                    stamp = state["FinishedAt"]

                    # Docker emits UTC RFC3339Nano; Python 3.9 only accepts fixed precision.
                    end = datetime.fromisoformat(stamp[:19]).replace(tzinfo=timezone.utc).timestamp()
                    end += float("0" + stamp[19:-1])
                    self.finish(attempt, state["ExitCode"], end - attempt["started_at"])
                else:
                    attempt["wall_time_estimated"] = True
                    elapsed = min(time.time() - attempt["started_at"], attempt["wall_limit_sec"] + 20)
                    self.finish(attempt, None, elapsed)

                self.remove_container(output)

    def finish(self, attempt: dict, code: int | None, wall_sec: float) -> dict:
        output = Path(attempt["output"])
        attempt.update(exit_code=code, wall_sec=wall_sec)
        usage = output / attempt["usage_file"] if attempt["usage_file"] is not None else None
        rows = read_json(usage)["requests"] if usage is not None and usage.exists() else []
        rows = [row for row in rows if row["kind"] == "request"]
        attempt["requests"] = len(rows)
        attempt["input_tokens"] = sum(row["input_tokens"] for row in rows)
        attempt["output_tokens"] = sum(row.get("generated_tokens", row["max_new_tokens"]) for row in rows)
        attempt["retained_bytes"] = self.retained_bytes()
        if attempt["retained_bytes"] > self.settings.budget["retained_mib"] * 1024**2:
            attempt["limit_reached"] = "retained_mib"
        artifact = output / attempt["artifact"]
        ready = code == 0 and artifact.exists() and read_json(artifact)["status"] == "completed"
        attempt["status"] = "completed" if code == 0 and ready and "limit_reached" not in attempt else "failed"
        if attempt["status"] == "completed":
            self.state[attempt["result_key"]] = str(artifact)
        write_json(self.state_path, self.state)
        return {
            "status": attempt["status"],
            attempt["result_key"]: str(artifact) if attempt["status"] == "completed" else None,
            "capture_state": str(self.state_path),
            "log": str(output / "run.log"),
        }

    def run(self, command: DeviceCommand) -> dict:
        remaining = self.remaining()
        if remaining["starts"] <= 0 or remaining["wall_sec"] <= 0:
            raise CaptureBudgetExceeded("Cumulative device-start or wall-time budget is exhausted; history is retained")
        for name in ("requests", "input_tokens", "output_tokens"):
            needed = getattr(command, name)
            if (needed is None and remaining[name] <= 0) or (needed is not None and needed > remaining[name]):
                raise CaptureBudgetExceeded(f"The shared capture exceeds the remaining {name} budget")
        if self.retained_bytes() >= remaining["retained_mib"] * 1024**2:
            raise CaptureBudgetExceeded("The cumulative capture storage budget is exhausted")

        with (Path(tempfile.gettempdir()) / "markov-device-capture.lock").open("a") as lease:
            fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
            waiting = time.monotonic()
            try:
                devices = self.runtime.wait_idle(waiting + remaining["wall_sec"])
            finally:
                self.record_processing(time.monotonic() - waiting)
            remaining = self.remaining()
            if remaining["wall_sec"] <= 0:
                raise CaptureBudgetExceeded(
                    "The cumulative wall-time budget was reached while waiting for idle devices"
                )

            output = self.directory / "captures" / str(len(self.state["attempts"]) + 1)
            output.mkdir(parents=True, exist_ok=False)
            (output / "device_before.txt").write_text(devices, encoding="utf-8")
            payload = dict(command.payload)
            if command.usage_file is not None:
                payload["budget"] = remaining
            write_json(output / "capture_input.json", payload)
            attempt = {
                "status": "running",
                "output": str(output),
                "started_at": time.time(),
                "wall_limit_sec": remaining["wall_sec"],
                "wall_sec": 0,
                "requests": 0,
                "input_tokens": 0,
                "output_tokens": 0,
                "artifact": command.artifact,
                "usage_file": command.usage_file,
                "result_key": command.result_key,
            }
            self.state["attempts"].append(attempt)
            write_json(self.state_path, self.state)
            begin = time.monotonic()
            process = None
            with (output / "run.log").open("w", encoding="utf-8") as log:
                try:
                    args = self.arguments(command, output, remaining["wall_sec"])
                    write_json(output / "invocation.json", args)
                    process = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
                    while process.poll() is None:
                        if (
                            time.monotonic() - begin > remaining["wall_sec"]
                            or self.retained_bytes() > remaining["retained_mib"] * 1024**2
                        ):
                            attempt["limit_reached"] = (
                                "wall_sec" if time.monotonic() - begin > remaining["wall_sec"] else "retained_mib"
                            )
                            break
                        try:
                            process.wait(timeout=2)
                        except subprocess.TimeoutExpired:
                            pass
                finally:
                    if process is not None and process.poll() is None:
                        self.remove_container(output)
                        process.terminate()
                        try:
                            process.wait(timeout=20)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=20)
                    result = self.finish(
                        attempt, process.returncode if process is not None else None, time.monotonic() - begin
                    )
                    self.remove_container(output)
            return result

    def arguments(self, command: DeviceCommand, output: Path, wall_sec: int) -> list[str]:
        args = [
            "docker",
            "run",
            "--init",
            "--network",
            "none",
            "--cidfile",
            str(output / "container.id"),
            "--memory",
            f"{self.settings.memory_limit_gib}g",
            "--memory-swap",
            f"{self.settings.memory_limit_gib}g",
            "--cpus",
            str(self.settings.cpus),
            "--pids-limit",
            "1024",
            "--shm-size",
            "8g",
        ]
        args += self.runtime.container_arguments()
        for source, target, writable in (
            *command.mounts,
            (Path(__file__).resolve().parents[2], "/opt/markov/python", False),
            (output, "/capture", True),
        ):
            args += ["--mount", f"type=bind,source={source},target={target}" + ("" if writable else ",readonly")]
        environment = json.loads(
            subprocess.check_output(
                ["docker", "image", "inspect", self.settings.image, "--format", "{{json .Config.Env}}"],
                text=True,
                timeout=20,
            )
        )
        paths = dict(value.split("=", 1) for value in environment).get("PYTHONPATH", "")
        for key, value in {
            "PYTHONPATH": "/opt/markov/python" + (":" + paths if paths else ""),
            "PYTHONDONTWRITEBYTECODE": "1",
            "OMP_NUM_THREADS": "1",
            "MKL_NUM_THREADS": "1",
            "OPENBLAS_NUM_THREADS": "1",
            **command.environment,
        }.items():
            args += ["-e", f"{key}={value}"]
        # Keep the stopped container until its exit and usage are recorded. The
        # in-container timeout still bounds an orphan if its host client dies.
        return args + [
            self.settings.image,
            "timeout",
            "--kill-after=20s",
            f"{wall_sec}s",
            "python3",
            "-m",
            command.module,
            *command.arguments,
        ]
