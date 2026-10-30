"""Own one trace or normal inference task with the same device and budget lifecycle."""

from dataclasses import asdict
import fcntl
import json
from pathlib import Path
import subprocess
import tempfile
import time

from markov.artifacts.request import PredictionRequest, read_json
from markov.artifacts.workload import Workload
from markov.artifacts.writing import write_json
from markov.adapters.ascend.capture import AscendDevices


class InferenceCapture:
    def __init__(self, request: PredictionRequest, profiling: bool = True, calibration: bool = False):
        self.request = request
        self.settings = request.execution.capture
        self.profiling = profiling
        self.calibration = calibration
        self.result_key = "manifest" if profiling else "calibration" if calibration else "observation"
        self.result_file = (
            "profile_manifest.json"
            if profiling
            else "region_measurements.json"
            if calibration
            else "normal_observation.json"
        )
        self.directory = request.execution.output / (
            "base" if profiling else "calibration" if calibration else "normal"
        )

    def run(self, recapture: bool = False) -> dict:
        if self.request.base.manifest is not None:
            if not self.profiling:
                raise ValueError("Normal measurement requires base.configuration; a trace is not a normal observation")
            if recapture:
                raise ValueError("Recapture needs base.configuration instead of an existing manifest")
            self.request.base.manifest.stat()
            return {"status": "reused", "manifest": str(self.request.base.manifest)}
        if self.settings is None:
            raise ValueError("Inference capture needs execution.capture with a runtime image, devices and budget")
        if (self.request.deployment.framework, self.request.deployment.runtime) != ("sglang", "ascend"):
            raise ValueError(
                "Inference capture currently supports SGLang on Ascend; other capture adapters are not restored"
            )
        if len(self.settings.devices) != self.request.deployment.tensor_parallel:
            raise ValueError("Single-host capture needs one declared device per tensor-parallel rank")
        model = Path(self.request.deployment.model)
        if not model.is_absolute() or not model.is_dir():
            raise ValueError("Inference capture needs deployment.model to name an absolute local model directory")

        from markov.adapters.sglang import capture as adapter

        workload = Workload.load(self.request.workload)
        server = adapter.server_arguments(self.request.deployment, self.request.base.configuration)
        self.runtime = AscendDevices(self.settings.devices)
        self.directory.mkdir(parents=True, exist_ok=True)
        state_path = self.directory / "capture_state.json"
        state = read_json(state_path) if state_path.exists() else {"attempts": []}
        if state.get(self.result_key) is not None and not recapture:
            raise ValueError(f"This task already completed at {state[self.result_key]}; request --recapture to repeat")
        if any(attempt["status"] == "running" for attempt in state["attempts"]):
            raise ValueError("An unfinished capture attempt needs process/usage reconciliation before restart")

        remaining = dict(self.settings.budget)
        remaining["starts"] -= len(state["attempts"])
        for attempt in state["attempts"]:
            for key in ("wall_sec", "requests", "input_tokens", "output_tokens"):
                remaining[key] -= attempt[key]
        remaining["wall_sec"] = int(remaining["wall_sec"])
        if min(remaining[key] for key in ("starts", "wall_sec", "requests", "input_tokens", "output_tokens")) <= 0:
            raise ValueError("Cumulative capture budget is exhausted; recorded attempts have not been reset")
        if (
            len(workload.requests) > remaining["requests"]
            or sum(row.max_new_tokens for row in workload.requests) > remaining["output_tokens"]
        ):
            raise ValueError("The workload exceeds the remaining request or output-token budget")
        if all(row.input_ids is not None for row in workload.requests):
            if sum(len(row.input_ids) for row in workload.requests) > remaining["input_tokens"]:
                raise ValueError("The workload exceeds the remaining input-token budget")
        if self.retained_bytes() >= remaining["retained_mib"] * 1024**2:
            raise ValueError("Retained capture data already reaches the cumulative storage budget")

        # This lease serializes Markov capture tasks; the hardware check also
        # protects unrelated jobs that do not participate in the lease.
        with (Path(tempfile.gettempdir()) / "markov-device-capture.lock").open("a") as lease:
            fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
            devices = self.runtime.require_idle()
            return self.collect(model, server, adapter, workload, state, state_path, remaining, devices)

    def collect(self, model, server, adapter, workload, state, state_path, remaining, devices) -> dict:
        output = self.directory / "captures" / str(len(state["attempts"]) + 1)
        output.mkdir(parents=True, exist_ok=False)
        (output / "device_before.txt").write_text(devices, encoding="utf-8")
        plan = {
            "deployment": asdict(self.request.deployment),
            "server": server,
            "environment": adapter.capture_environment(self.profiling, self.calibration),
            "profiling": self.profiling,
            "calibration": self.calibration,
            "workload": asdict(workload),
            "budget": remaining,
        }
        write_json(output / "capture_input.json", plan)
        cid = output / "container.id"
        args = [
            "docker",
            "run",
            "--rm",
            "--init",
            "--network",
            "none",
            "--cidfile",
            str(cid),
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
            (model, "/model", False),
            (Path(__file__).resolve().parents[2], "/opt/markov/python", False),
            (output, "/capture", True),
        ):
            args += ["--mount", f"type=bind,source={source},target={target}" + ("" if writable else ",readonly")]
        # Preserve the runtime SDK's Python paths while selecting this worktree's adapter.
        image_environment = json.loads(
            subprocess.check_output(
                ["docker", "image", "inspect", self.settings.image, "--format", "{{json .Config.Env}}"], text=True
            )
        )
        image_paths = dict(item.split("=", 1) for item in image_environment).get("PYTHONPATH", "")
        environment = {
            "PYTHONPATH": "/opt/markov/python" + (":" + image_paths if image_paths else ""),
            "PYTHONDONTWRITEBYTECODE": "1",
            "OMP_NUM_THREADS": "1",
            "MKL_NUM_THREADS": "1",
            "OPENBLAS_NUM_THREADS": "1",
            **plan["environment"],
        }
        for key, value in environment.items():
            args += ["-e", f"{key}={value}"]
        args += [
            self.settings.image,
            "python3",
            "-m",
            adapter.__name__,
            "collect",
            "--input",
            "/capture/capture_input.json",
        ]
        write_json(output / "invocation.json", args)
        attempt = {
            "status": "running",
            "output": str(output),
            "started_at": time.time(),
            "wall_sec": 0,
            "requests": 0,
            "input_tokens": 0,
            "output_tokens": 0,
        }
        state["attempts"].append(attempt)
        write_json(state_path, state)
        begin = time.monotonic()
        with (output / "run.log").open("w") as log:
            process = None
            try:
                process = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
                while process.poll() is None:
                    retained = self.retained_bytes()
                    if (
                        time.monotonic() - begin > remaining["wall_sec"]
                        or retained > remaining["retained_mib"] * 1024**2
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
                if process is not None and process.poll() != 0:
                    if cid.exists():
                        subprocess.run(
                            ["docker", "rm", "-f", cid.read_text().strip()],
                            stdout=log,
                            stderr=subprocess.STDOUT,
                            timeout=20,
                        )
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=20)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=20)
                attempt["wall_sec"] = time.monotonic() - begin
                attempt["exit_code"] = process.returncode if process is not None else None
                report = output / "workload_report.json"
                rows = read_json(report)["requests"] if report.exists() else []
                attempt["requests"] = len(rows)
                attempt["input_tokens"] = sum(row["input_tokens"] for row in rows)
                attempt["output_tokens"] = sum(row.get("generated_tokens", row["max_new_tokens"]) for row in rows)
                attempt["retained_bytes"] = self.retained_bytes()
                if attempt["retained_bytes"] > remaining["retained_mib"] * 1024**2:
                    attempt["limit_reached"] = "retained_mib"
                artifact = output / self.result_file
                ready = artifact.exists()
                if ready:
                    content = read_json(artifact)
                    ready = (
                        content["profiling_ready"] is True
                        if self.profiling
                        else content["profiling"] is False and content["status"] == "completed"
                    )
                attempt["status"] = (
                    "completed" if attempt["exit_code"] == 0 and ready and "limit_reached" not in attempt else "failed"
                )
                if attempt["status"] == "completed":
                    state[self.result_key] = str(artifact)
                try:
                    after = self.runtime.snapshot()
                    (output / "device_after.txt").write_text(after, encoding="utf-8")
                finally:
                    write_json(state_path, state)
        result = {
            "status": attempt["status"],
            self.result_key: str(artifact) if attempt["status"] == "completed" else None,
            "capture_state": str(state_path),
        }
        if attempt["status"] != "completed":
            result["log"] = str(output / "run.log")
        return result

    def retained_bytes(self):
        total = 0
        for path in self.directory.rglob("*"):
            try:
                if path.is_file():
                    total += path.stat().st_size
            except FileNotFoundError:
                # The native exporter may retire a temporary file while counted.
                continue
        return total
