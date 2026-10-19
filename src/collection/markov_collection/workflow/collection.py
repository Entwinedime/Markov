"""Container orchestration for explicit, serial, budgeted native collection."""

from __future__ import annotations

import fcntl
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import read_json, write_json
from markov_collection.workflow.container_environment import ContainerEnvironment
from markov_collection.workflow.modeling_job import ModelingJob, ModelingJobFailed
from markov_collection.workflow.run_directory import RunDirectory


class CaptureProcessingFailed(RuntimeError):
    """A processor published a terminal failure for the recorded attempt."""


class CollectionWorkflow:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.environment = ContainerEnvironment(root)

    def _preparation_mounts(
        self, project: Path, measurements: Path | None, communication: bool, prediction_result: Path | None
    ) -> list[str]:
        # Discover file dependencies only. The C++ reader owns input validation
        # and framework semantics; host absolute paths keep their declared names.
        target = Path(self.environment.container_path(project))
        inputs = read_json(project)
        paths = {target: project}
        for key, field in (("configurations", "config"), ("request_workloads", "path")):
            for item in inputs[key]:
                paths[target.parent / item[field]] = self.environment.host_path(str(project.parent / item[field]))
        if measurements is not None:
            paths[Path(self.environment.container_path(measurements))] = measurements

        if prediction_result is not None:
            paths[Path(self.environment.container_path(prediction_result))] = prediction_result
            prediction = read_json(prediction_result)
            for combination in prediction["combinations"]:
                requirements = combination.get("performance_requirements")
                if requirements is not None and "cpu_service_catalog" in requirements:
                    catalog = Path(requirements["cpu_service_catalog"])
                    paths[catalog] = self.environment.host_path(str(catalog))

            # Planning reads provenance and native cohorts, never raw timing
            # exports. Missing optional history stays unknown in the C++ plan.
            manifests = {
                Path(os.path.normpath(target.parent / reference))
                for reference in inputs.get("assets", {}).get("performance_manifests", [])
            }
            for model in prediction["cost_models"]:
                if model["status"] == "loaded":
                    manifests.add(Path(model["source_manifest"]))

            for manifest_path in sorted(manifests):
                source = self.environment.host_path(str(manifest_path))
                if not source.is_file():
                    continue

                paths[manifest_path] = source
                manifest = read_json(source)
                if "input" in manifest:
                    prepared = self.environment.host_path(str(source.parent / manifest["input"]))
                    if prepared.is_file():
                        paths[manifest_path.parent / manifest["input"]] = prepared

        if communication:
            for reference in inputs.get("assets", {}).get("performance_manifests", []):
                manifest_path = Path(os.path.normpath(target.parent / reference))
                source = self.environment.host_path(str(project.parent / reference))
                paths[manifest_path] = source
                manifest = read_json(source)
                if manifest["measurement_kind"] == "native_device_work":
                    for field in ("input", "device_measurements"):
                        paths[manifest_path.parent / manifest[field]] = self.environment.host_path(
                            str(source.parent / manifest[field])
                        )

        binary = self.root / "build/markov/markov"
        mounts = ["--mount", f"type=bind,src={binary},dst={self.environment.container_path(binary)},readonly"]
        paths = {Path(os.path.normpath(path)): source for path, source in paths.items()}
        for path, source in sorted(paths.items()):
            source = source.resolve()
            mounts += ["--mount", f"type=bind,src={source},dst={path},readonly"]
        return mounts

    def prepare(
        self,
        project: Path,
        role: str,
        measurement_kind: str,
        measurement_requests: Path | None = None,
        *,
        configuration_id: str | None = None,
        workload_id: str | None = None,
        budget_sec: int | None = None,
        purpose: str | None = None,
        representative: bool = False,
        prediction_result: Path | None = None,
    ) -> dict[str, Any]:
        command = "prepare" if role == "base" else "prepare-sampling"
        if representative:
            command = "prepare-representative"
        if role == "validation":
            command = "prepare-target"
        if role == "prediction":
            command = "prepare-prediction"
        if role == "performance_plan":
            command = "prepare-performance"
        if role == "performance" and measurement_kind == "communication":
            command = "prepare-communication"

        arguments = [command, "--project", self.environment.container_path(project)]
        if configuration_id is not None:
            arguments += ["--configuration", configuration_id]
        if workload_id is not None:
            arguments += ["--workload", workload_id]
        if role == "validation":
            arguments += ["--budget-sec", str(budget_sec)]
            if purpose is not None:
                arguments += ["--purpose", purpose]
        if measurement_requests is not None:
            arguments += ["--measurement-requests", self.environment.container_path(measurement_requests)]
        if prediction_result is not None:
            arguments += ["--prediction-result", self.environment.container_path(prediction_result)]

        result = subprocess.run(
            [
                "docker",
                "run",
                "--rm",
                "--pull=never",
                "--network=none",
                *self._preparation_mounts(
                    project, measurement_requests, command == "prepare-communication", prediction_result
                ),
                "--workdir",
                "/workspace/trace-sim",
                self.environment.service_image("modeling"),
                "build/markov/markov",
                *arguments,
            ],
            check=True,
            text=True,
            capture_output=True,
        )
        return json.loads(result.stdout)

    def profile(
        self,
        project: Path,
        *,
        role: str = "base",
        repetitions: int = 3,
        measurement_kind: str = "transfer",
        measurement_requests: Path | None = None,
        configuration_id: str | None = None,
        workload_id: str | None = None,
        budget_sec: int | None = None,
        purpose: str | None = None,
        representative: bool = False,
    ) -> dict[str, Any]:
        prepared = self.prepare(
            project,
            role,
            measurement_kind,
            measurement_requests,
            configuration_id=configuration_id,
            workload_id=workload_id,
            budget_sec=budget_sec,
            purpose=purpose,
            representative=representative,
        )
        if role == "performance":
            prepared["repetitions"] = repetitions
            prepared["measurement_kind"] = measurement_kind

        run = RunDirectory.create(
            self.environment.host_path(prepared["output_directory"]), prepared["framework"], prepared["model_path"]
        )
        directory = run.root
        state: dict[str, Any] = {
            "id": run.identity,
            "project": str(project),
            "framework": prepared["framework"],
            "collection": {
                kind: {"consumed_wall_time_ns": 0, **permission} for kind, permission in prepared["permissions"].items()
            },
            "attempts": [],
        }

        write_json(directory / "inputs/prepared_input.json", prepared)
        self.select(prepared, directory, state)
        write_json(directory / "run.json", state)
        with (directory / "run.lock").open("a", encoding="utf-8") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            self.collect(directory, state)

        return self._result(directory, state)

    def select(
        self,
        prepared: dict[str, Any],
        directory: Path,
        state: dict[str, Any],
        *,
        max_wall_time_sec: int | None = None,
    ) -> None:
        """Register one capture without changing its parent's prediction phase."""
        write_json(directory / "work/prepared_input.json", prepared)
        state["capture"] = {
            "role": prepared["role"],
            "attempt_index": len(state["attempts"]),
            "phase": "planned",
            "status": "needs_data",
            "manifest": None,
            "max_wall_time_sec": max_wall_time_sec,
        }

    def collect(self, directory: Path, state: dict[str, Any]) -> None:
        acquisition = state["capture"]
        if acquisition["manifest"] is None:
            if len(state["attempts"]) == acquisition["attempt_index"]:
                self._collect(read_json(directory / "work/prepared_input.json"), directory, state)
            elif state["attempts"][-1]["status"] == "prepared":
                self._launch(directory, state)
            elif state["attempts"][-1]["status"] == "completed":
                self._finalize(directory, state)

        # The parent ledger now owns the manifest. Completed processor receipts
        # can disappear; a failed or still active processor keeps its work.
        write_json(directory / "run.json", state)
        if acquisition["manifest"] is not None:
            (directory / "work/prepared_input.json").unlink(missing_ok=True)
            RunDirectory(directory, state["id"]).consume_work("processing", acquisition["attempt_index"] + 1)

    def _collect(self, prepared: dict[str, Any], directory: Path, state: dict[str, Any]) -> None:
        acquisition = state["capture"]
        role = acquisition["role"]
        permission = state["collection"][role]
        if not permission["allow_run"]:
            acquisition.update(phase="finished", status="needs_data", reason=f"{role}_collection_disabled")
            write_json(directory / "run.json", state)
            return

        prepared["permissions"] = {
            kind: {"allow_run": value["allow_run"], "max_wall_time_sec": value["max_wall_time_sec"]}
            for kind, value in state["collection"].items()
        }
        prepared["permission"] = prepared["permissions"][role]

        # Retries share the task budget; a new attempt does not reset consumed time.
        remaining_ns = permission["max_wall_time_sec"] * 1_000_000_000 - permission["consumed_wall_time_ns"]
        limit = acquisition["max_wall_time_sec"]
        if limit is not None:
            remaining_ns = min(remaining_ns, limit * 1_000_000_000)
            prepared["permission"] = {**prepared["permission"], "max_wall_time_sec": limit}
        if remaining_ns <= 0:
            acquisition.update(phase="finished", status="needs_data", reason=f"{role}_collection_budget_exhausted")
            write_json(directory / "run.json", state)
            return

        environment = prepared["environment"]
        native_entrypoint = self.environment.native_entrypoint(environment["service"])
        run = RunDirectory(directory, state["id"])
        ordinal = len(state["attempts"]) + 1
        attempt_directory = run.capture(role, ordinal)
        # Before the attempt is registered, an interrupted preparation may have
        # left this directory; no native process has been launched for it yet.
        attempt_directory.mkdir(parents=True, exist_ok=True)
        write_json(attempt_directory / "prepared_input.json", prepared)
        container = run.container(f"capture-{ordinal:03d}")
        attempt = {
            "role": role,
            "measurement_kind": prepared["measurement_kind"] if role == "performance" else None,
            "directory": str(attempt_directory),
            "processing_directory": str(run.work("processing", ordinal)),
            "container": container,
            "created_at_ns": time.time_ns(),
            "status": "prepared",
            "pid": None,
        }

        # Persist the attempt identity before launch so resume can recover its container.
        state["attempts"].append(attempt)
        acquisition["phase"] = "planned"
        acquisition["status"] = "needs_data"
        acquisition.pop("reason", None)
        write_json(
            attempt_directory / "attempt_input.json",
            {
                "framework": prepared["framework"],
                "role": role,
                "measurement_kind": prepared["measurement_kind"] if role == "performance" else None,
                "observation_purpose": prepared["observation_purpose"] if role == "validation" else None,
                "created_at_ns": attempt["created_at_ns"],
                "remaining_wall_time_ns": remaining_ns,
                "native_entrypoint": native_entrypoint,
                "log": self.environment.container_path(run.log(f"native-{ordinal:03d}")),
            },
        )

        write_json(directory / "run.json", state)
        self._launch(directory, state)

    def _launch(self, directory: Path, state: dict[str, Any]) -> None:
        acquisition = state["capture"]
        attempt = state["attempts"][-1]
        attempt_directory = Path(attempt["directory"])
        prepared = read_json(attempt_directory / "prepared_input.json")
        environment = prepared["environment"]
        run = RunDirectory(directory, state["id"])
        ordinal = len(state["attempts"])
        container = attempt["container"]

        # Registration is durable before launch. A host may stop between them;
        # time spent waiting for a later resume is not container bootstrap time.
        configuration = read_json(attempt_directory / "attempt_input.json")
        configuration["created_at_ns"] = time.time_ns()
        write_json(attempt_directory / "attempt_input.json", configuration)
        remaining_ns = configuration["remaining_wall_time_ns"]
        attempt.update(status="collecting", created_at_ns=configuration["created_at_ns"], pid=None)
        acquisition.update(phase="collecting", status="needs_data")
        write_json(directory / "run.json", state)

        command = self.environment.compose + [
            "run",
            "-d",
            "-T",
            "--no-deps",
            "--name",
            container,
            "--entrypoint",
            "python3",
            "-e",
            "ASCEND_VISIBLE_DEVICES=" + ",".join(str(device) for device in environment["devices"]),
            "-e",
            "PYTHONPATH=/workspace/trace-sim/src/collection",
            environment["service"],
            "-m",
            "markov_collection.workflow.attempt",
            "--attempt",
            self.environment.container_path(attempt_directory / "attempt_input.json"),
        ]
        existing = self.environment.container_state(container)
        if existing is not None:
            # A created container reserves the same identity but has not run its
            # monitor. Resume starts it rather than creating another attempt.
            command = ["docker", "start", container]

        started_ns = time.perf_counter_ns()
        launch_log = run.log(f"launch-{ordinal:03d}")
        launch_log.parent.mkdir(parents=True, exist_ok=True)
        print(
            f"Capturing {acquisition['role']}, attempt {ordinal}; log: {run.log(f'native-{ordinal:03d}')}",
            file=sys.stderr,
            flush=True,
        )
        with launch_log.open("w", encoding="utf-8") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            attempt["pid"] = process.pid
            write_json(directory / "run.json", state)
            try:
                code = process.wait(timeout=remaining_ns / 1_000_000_000)
                launch_result = {
                    "return_code": code,
                    "elapsed_wall_time_ns": time.perf_counter_ns() - started_ns,
                    "ended_at_ns": time.time_ns(),
                }
                write_json(attempt_directory / "launch_result.json", launch_result)
                container_state = self.environment.container_state(container)
                failed_start = container_state is None or (
                    container_state["Status"] == "created" and bool(container_state["Error"])
                )
                if code != 0 and failed_start:
                    self._finish_attempt(
                        state, "failed", code, launch_result["elapsed_wall_time_ns"], launch_result["ended_at_ns"]
                    )
                else:
                    subprocess.run(["docker", "wait", container], check=True, stdout=log, stderr=subprocess.STDOUT)
                    self.reconcile(state)
            except subprocess.TimeoutExpired:
                self._stop(container, process, log)
                if self.environment.container_state(container) is None:
                    self._finish_attempt(
                        state, "budget_exhausted", 124, time.perf_counter_ns() - started_ns, time.time_ns()
                    )
                else:
                    self.reconcile(state)
            except KeyboardInterrupt:
                self._stop(container, process, log)
                if self.environment.container_state(container) is None:
                    self._finish_attempt(state, "interrupted", 130, time.perf_counter_ns() - started_ns, time.time_ns())
                else:
                    self.reconcile(state)
                raise
            finally:
                write_json(directory / "run.json", state)

        if acquisition["status"] == "captured":
            self._finalize(directory, state)

    def _finalize(self, directory: Path, state: dict[str, Any]) -> None:
        acquisition = state["capture"]
        failure = self._reconcile_processing(state)
        write_json(directory / "run.json", state)
        if failure is not None:
            raise CaptureProcessingFailed(failure["message"])
        if acquisition["manifest"] is not None:
            return

        if acquisition["role"] == "performance":
            prepared = read_json(Path(state["attempts"][-1]["directory"]) / "prepared_input.json")
            if prepared["measurement_kind"] in {"transfer", "cpu"}:
                raise RuntimeError("native host sampler exited without publishing its performance manifest")

        acquisition["phase"] = "processing"

        write_json(directory / "run.json", state)
        attempt = state["attempts"][-1]
        capture = Path(attempt["directory"]) / "native_capture.json"
        run = RunDirectory(directory, state["id"])
        ordinal = len(state["attempts"])
        task = f"processing-{ordinal:03d}"
        print(f"Processing {acquisition['role']}; log: {run.log(task)}", file=sys.stderr, flush=True)
        work = Path(attempt["processing_directory"])
        job = ModelingJob(self.environment, run.container(task), work, run.log(task))
        arguments = [
            "python3",
            "-m",
            "markov_collection.workflow.processing",
            "--capture",
            self.environment.container_path(capture),
            "--role",
            acquisition["role"],
            "--directory",
            self.environment.container_path(work),
        ]
        outputs = {capture.parent}
        if acquisition["role"] == "base":
            prepared = read_json(capture.parent / "prepared_input.json")
            templates = run.asset("templates", ordinal)
            intermediates = run.diagnostics("base", ordinal) if prepared["diagnostics"] else work / "source"
            outputs.update({templates, intermediates})
            arguments += [
                "--template-directory",
                self.environment.container_path(templates),
                "--work-directory",
                self.environment.container_path(intermediates),
            ]

        try:
            job.run(arguments, {self.root / "build/markov/markov", self.root / "src/collection"}, outputs)
            self._reconcile_processing(state)
            if acquisition["manifest"] is None:
                raise RuntimeError("capture processor exited without publishing its result")
        except (subprocess.CalledProcessError, ModelingJobFailed):
            failure = self._reconcile_processing(state)
            if failure is not None:
                write_json(directory / "run.json", state)
                raise CaptureProcessingFailed(failure["message"]) from None
            acquisition.update(phase="interrupted", reason=f"{acquisition['role']}_processing_interrupted")
            write_json(directory / "run.json", state)
            raise
        except (Exception, KeyboardInterrupt):
            acquisition.update(phase="interrupted", reason=f"{acquisition['role']}_processing_interrupted")
            write_json(directory / "run.json", state)
            raise

        write_json(directory / "run.json", state)

    def _reconcile_processing(self, state: dict[str, Any]) -> dict[str, Any] | None:
        acquisition = state["capture"]
        role = acquisition["role"]
        if (
            len(state["attempts"]) <= acquisition["attempt_index"]
            or state["attempts"][-1]["status"] != "completed"
            or acquisition["manifest"] is not None
        ):
            return None

        attempt = state["attempts"][-1]
        directory = Path(attempt["directory"])
        try:
            failure = read_json(Path(attempt["processing_directory"]) / "processing_failure.json")
        except FileNotFoundError:
            failure = None
        if failure is not None:
            attempt["processing_failure"] = failure
            acquisition.update(phase="finished", status="failed", reason=f"{role}_processing_failed")
            return failure

        # Native completion and processing outcome are separate facts. Status and
        # resume consume the same terminal records without launching another job.
        native_host = role == "performance" and attempt["measurement_kind"] in {"transfer", "cpu"}
        result_path = (
            directory / "performance_manifest.json"
            if native_host
            else Path(attempt["processing_directory"]) / "processing_result.json"
        )
        try:
            result = read_json(result_path)
        except FileNotFoundError:
            return None

        manifest = (
            directory / "performance_manifest.json"
            if native_host
            else self.environment.host_path(result[f"{role}_manifest"])
        )
        acquisition.update(manifest=str(manifest), readiness=result["readiness"])
        attempt.pop("processing_failure", None)
        acquisition.update(phase="finished", status="captured")
        acquisition.pop("reason", None)

        return None

    def resume(self, directory: Path, *, budgets: dict[str, int] | None = None) -> dict[str, Any]:
        with (directory / "run.lock").open("a", encoding="utf-8") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                return {**self.status(directory), "workflow_busy": True}

            state = read_json(directory / "run.json")
            live, active_ns = self.reconcile(state)
            write_json(directory / "run.json", state)
            if live:
                result = {
                    **state,
                    "run": str(directory),
                    "task_status": state["capture"]["phase"],
                    "status": state["capture"]["status"],
                    "live_attempt": True,
                    "active_wall_time_ns": active_ns,
                }
                if budgets:
                    result["requested_changes_applied"] = False
                return result

            if budgets:
                self._revise_budget(directory, state, budgets)
            self.retry_native(directory, state)
            self.collect(directory, state)

        return self._result(directory, state)

    def retry_native(self, directory: Path, state: dict[str, Any]) -> None:
        # Only an explicit resume retries a terminated native acquisition.
        # Selection preserves the failed attempt and its consumed budget.
        acquisition = state["capture"]
        permission = state["collection"][acquisition["role"]]
        if (
            not permission["allow_run"]
            or permission["consumed_wall_time_ns"] >= permission["max_wall_time_sec"] * 1_000_000_000
        ):
            return

        if (
            state["attempts"]
            and state["attempts"][-1]["status"] in {"failed", "interrupted", "budget_exhausted"}
            and len(state["attempts"]) > acquisition["attempt_index"]
        ):
            self.select(
                read_json(Path(state["attempts"][-1]["directory"]) / "prepared_input.json"),
                directory,
                state,
                max_wall_time_sec=acquisition["max_wall_time_sec"],
            )

    def _revise_budget(self, directory: Path, state: dict[str, Any], budgets: dict[str, int]) -> None:
        role = state["capture"]["role"]
        if set(budgets) != {role}:
            raise ValueError(f"this {role} collection task only accepts its own budget")

        seconds = budgets[role]
        if seconds <= 0 or seconds * 1_000_000_000 < state["collection"][role]["consumed_wall_time_ns"]:
            raise ValueError(f"{role} total budget must be positive and cover its consumed time")
        prepared = read_json(directory / "inputs/prepared_input.json")
        if prepared["environment"] is None:
            raise ValueError("enabled collection requires the task's declared environment")

        permission = {"allow_run": True, "max_wall_time_sec": seconds}
        state["collection"][role].update(permission)

        write_json(directory / "run.json", state)

    def _live_launcher(self, attempt: dict[str, Any]) -> bool:
        if attempt["pid"] is None:
            return False

        process = subprocess.run(
            ["ps", "-p", str(attempt["pid"]), "-o", "args="],
            capture_output=True,
            text=True,
            check=False,
        )
        return process.returncode == 0 and attempt["container"] in process.stdout

    def _finish_attempt(self, state: dict[str, Any], status: str, code: int, elapsed_ns: int, ended_ns: int) -> None:
        acquisition = state["capture"]
        attempt = state["attempts"][-1]
        attempt.update(status=status, return_code=code, elapsed_wall_time_ns=elapsed_ns, ended_at_ns=ended_ns)

        role = acquisition["role"]
        state["collection"][role]["consumed_wall_time_ns"] += elapsed_ns

        acquisition.update(phase="finished", status="needs_data")
        if status == "completed":
            acquisition.update(phase="processing", status="captured")
        elif status == "failed":
            acquisition.update(status="failed", reason=f"{role}_collection_failed")
        elif status == "interrupted":
            acquisition.update(phase="interrupted", reason=f"{role}_collection_interrupted")
        else:
            acquisition["reason"] = f"{role}_collection_budget_exhausted"

    def reconcile(self, state: dict[str, Any]) -> tuple[bool, int]:
        if not state["attempts"] or state["attempts"][-1]["status"] not in {"prepared", "collecting"}:
            return False, 0

        attempt = state["attempts"][-1]
        container = self.environment.container_state(attempt["container"])
        try:
            record = read_json(Path(attempt["directory"]) / "attempt_status.json")
        except FileNotFoundError:
            record = None  # The container has not entered its monitor yet.

        if container is None or container["Status"] == "created":
            if self._live_launcher(attempt):
                return True, time.time_ns() - attempt["created_at_ns"]
            try:
                launch = read_json(Path(attempt["directory"]) / "launch_result.json")
            except FileNotFoundError:
                launch = None

            if record is not None and record["ended_at_ns"] is not None:
                self._finish_attempt(
                    state,
                    record["status"],
                    record["return_code"],
                    record["elapsed_wall_time_ns"],
                    record["ended_at_ns"],
                )
                return False, 0

            if record is None and launch is None:
                # This workflow retains native containers. An absent handle and
                # no monitor/launcher outcome means launch has not completed.
                # Reuse its name: Docker also prevents competing creates from
                # launching a second native job after a lost host.
                attempt["status"] = "prepared"
                state["capture"]["phase"] = "planned"
                return False, 0

            # A failed launcher plus an absent/failed-created container proves
            # no monitor ran. Charge the recorded launch interval, never time
            # accumulated while the task waited for a later resume.
            failed_start = container is None or bool(container["Error"])
            if launch is not None and launch["return_code"] != 0 and failed_start:
                self._finish_attempt(
                    state, "failed", launch["return_code"], launch["elapsed_wall_time_ns"], launch["ended_at_ns"]
                )
                return False, 0

            raise RuntimeError(
                "native attempt has no running handle or terminal record; its budget cannot be recovered"
            )

        if container["Running"]:
            if record is None:
                return True, time.time_ns() - attempt["created_at_ns"]
            active_ns = record["boot_wall_time_ns"]
            if record["admitted_at_ns"] is not None:
                end_ns = record["ended_at_ns"] if record["ended_at_ns"] is not None else time.time_ns()
                active_ns += end_ns - record["admitted_at_ns"]
            return True, active_ns

        if record is not None and record["ended_at_ns"] is not None:
            self._finish_attempt(
                state, record["status"], record["return_code"], record["elapsed_wall_time_ns"], record["ended_at_ns"]
            )
        else:
            # PID 1 may have received SIGKILL. Docker retains the actual end time;
            # elapsed time never includes the later delay before resume/status.
            seconds, _, fraction = container["FinishedAt"].removesuffix("Z").partition(".")
            ended_ns = int(datetime.strptime(seconds, "%Y-%m-%dT%H:%M:%S").replace(tzinfo=timezone.utc).timestamp())
            ended_ns = ended_ns * 1_000_000_000 + int(fraction.ljust(9, "0"))
            elapsed_ns = ended_ns - attempt["created_at_ns"]
            if record is not None:
                elapsed_ns = record["boot_wall_time_ns"]
                if record["admitted_at_ns"] is not None:
                    elapsed_ns += ended_ns - record["admitted_at_ns"]
            self._finish_attempt(state, "interrupted", container["ExitCode"], elapsed_ns, ended_ns)

        return False, 0

    def _result(self, directory: Path, state: dict[str, Any]) -> dict[str, Any]:
        live, active_ns = self.reconcile(state)
        self._reconcile_processing(state)
        acquisition = state["capture"]
        return {
            **state,
            "run": str(directory),
            "task_status": acquisition["phase"],
            "status": acquisition["status"],
            "live_attempt": live,
            "active_wall_time_ns": active_ns,
        }

    def status(self, directory: Path) -> dict[str, Any]:
        return self._result(directory, read_json(directory / "run.json"))

    def _stop(self, container: str, process: subprocess.Popen[Any], log: Any) -> None:
        if process.poll() is None:
            process.terminate()
        process.wait()
        if self.environment.container_state(container) is not None:
            subprocess.run(
                ["docker", "stop", "--time", "30", container], stdout=log, stderr=subprocess.STDOUT, check=True
            )
