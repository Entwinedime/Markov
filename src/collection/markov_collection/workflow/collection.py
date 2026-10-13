"""Container orchestration for explicit, serial, budgeted native collection."""

from __future__ import annotations

import argparse
import fcntl
import json
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any
from uuid import uuid4

from markov_collection.artifacts.json_file import write_json


class CaptureProcessingFailed(RuntimeError):
    """A processor published a terminal failure for the recorded attempt."""


class CollectionWorkflow:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.compose = [
            "docker",
            "compose",
            "-p",
            "markov-next-redesign",
            "-f",
            str(root / "docker/compose/inference.yml"),
        ]

    def _host_path(self, path: str) -> Path:
        declared = Path(path)
        if declared.is_relative_to("/workspace/trace-sim"):
            return self.root / declared.relative_to("/workspace/trace-sim")
        return declared

    def _container_path(self, path: Path) -> str:
        if path.is_relative_to(self.root):
            return str(Path("/workspace/trace-sim") / path.relative_to(self.root))
        # External declared inputs retain their absolute path. The consuming
        # operation mounts each required file explicitly, with its own access mode.
        return str(path)

    def _save(self, path: Path, value: Any) -> None:
        write_json(path, value)

    def _prepare(
        self,
        project: Path,
        role: str,
        measurement_kind: str,
        measurement_requests: Path | None = None,
        *,
        configuration_id: str | None = None,
        workload_id: str | None = None,
        budget_sec: int | None = None,
        representative: bool = False,
    ) -> dict[str, Any]:
        command = "prepare" if role == "base" else "prepare-sampling"
        if representative:
            command = "prepare-representative"
        if role == "validation":
            command = "prepare-target"
        if role == "prediction":
            command = "prepare-prediction"
        if role == "performance" and measurement_kind == "communication":
            command = "prepare-communication"

        arguments = [command, "--project", self._container_path(project)]
        if configuration_id is not None:
            arguments += ["--configuration", configuration_id]
        if workload_id is not None:
            arguments += ["--workload", workload_id]
        if role == "validation":
            arguments += ["--budget-sec", str(budget_sec)]
        if measurement_requests is not None:
            arguments += ["--measurement-requests", self._container_path(measurement_requests)]

        result = subprocess.run(
            [
                "docker",
                "run",
                "--rm",
                "--pull=never",
                "--network=none",
                "--mount",
                f"type=bind,src={self.root},dst=/workspace/trace-sim,readonly",
                "--workdir",
                "/workspace/trace-sim",
                self._service_image("modeling"),
                "build/markov/markov",
                *arguments,
            ],
            check=True,
            text=True,
            capture_output=True,
        )
        return json.loads(result.stdout)

    def _service_image(self, service: str) -> str:
        definition = subprocess.run(
            self.compose + ["config", "--format", "json"], check=True, text=True, capture_output=True
        )
        return json.loads(definition.stdout)["services"][service]["image"]

    def _native_entrypoint(self, service: str) -> list[str]:
        image = self._service_image(service)
        inspection = subprocess.run(
            ["docker", "image", "inspect", "--format", "{{json .Config.Entrypoint}}", image],
            check=True,
            text=True,
            capture_output=True,
        )
        return json.loads(inspection.stdout)

    def profile(
        self,
        project: Path,
        *,
        role: str = "base",
        repetitions: int = 3,
        measurement_kind: str = "cpu",
        measurement_requests: Path | None = None,
        configuration_id: str | None = None,
        workload_id: str | None = None,
        budget_sec: int | None = None,
        representative: bool = False,
    ) -> dict[str, Any]:
        prepared = self._prepare(
            project,
            role,
            measurement_kind,
            measurement_requests,
            configuration_id=configuration_id,
            workload_id=workload_id,
            budget_sec=budget_sec,
            representative=representative,
        )
        if role == "performance":
            prepared["repetitions"] = repetitions
            prepared["measurement_kind"] = measurement_kind

        # A timestamp alone can collide across independent project directories.
        run_name = f"{datetime.now():run-%Y%m%d-%H%M%S-%f}-{uuid4().hex}"
        directory = self._host_path(prepared["output_directory"]) / run_name
        directory.mkdir(parents=True)
        state: dict[str, Any] = {
            "project": str(project),
            "task_status": "planned",
            "status": "needs_data",
            "framework": prepared["framework"],
            "role": role,
            "base_manifest": None,
            "performance_manifest": None,
            "collection": {
                kind: {"consumed_wall_time_ns": 0, **permission} for kind, permission in prepared["permissions"].items()
            },
            "attempts": [],
        }

        state[f"{role}_manifest"] = None
        if role == "validation":
            state["candidate_id"] = prepared["candidate_id"]

        self._save(directory / "prepared_input.json", prepared)

        self._save(directory / "run.json", state)
        if not prepared["permission"]["allow_run"]:
            state.update(task_status="finished", status="needs_data", reason=f"{role}_collection_disabled")
            self._save(directory / "run.json", state)
            return {"run": str(directory), **state}

        with (directory / "run.lock").open("a", encoding="utf-8") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            self._collect(prepared, directory, state)

        return {"run": str(directory), **state}

    def _collect(self, prepared: dict[str, Any], directory: Path, state: dict[str, Any]) -> None:
        role = state["role"]
        permission = state["collection"][role]
        prepared["permissions"] = {
            kind: {"allow_run": value["allow_run"], "max_wall_time_sec": value["max_wall_time_sec"]}
            for kind, value in state["collection"].items()
        }
        prepared["permission"] = prepared["permissions"][role]

        # Retries share the task budget; a new attempt does not reset consumed time.
        remaining_ns = permission["max_wall_time_sec"] * 1_000_000_000 - permission["consumed_wall_time_ns"]
        if remaining_ns <= 0:
            state.update(task_status="finished", status="needs_data", reason=f"{role}_collection_budget_exhausted")
            self._save(directory / "run.json", state)
            return

        environment = prepared["environment"]
        native_entrypoint = self._native_entrypoint(environment["service"])
        attempt_directory = directory / role / f"attempt-{len(state['attempts']) + 1}"
        attempt_directory.mkdir(parents=True)
        self._save(attempt_directory / "prepared_input.json", prepared)
        container = "markov-capture-" + directory.name + f"-{len(state['attempts']) + 1}"
        attempt = {
            "role": role,
            "directory": str(attempt_directory),
            "container": container,
            "created_at_ns": time.time_ns(),
            "status": "collecting",
            "pid": None,
        }

        # Persist the attempt identity before launch so resume can recover its container.
        state["attempts"].append(attempt)
        state["task_status"] = "collecting"
        state["status"] = "needs_data"
        state.pop("reason", None)
        self._save(
            attempt_directory / "attempt_input.json",
            {
                "framework": prepared["framework"],
                "role": role,
                "measurement_kind": prepared["measurement_kind"] if role == "performance" else None,
                "created_at_ns": attempt["created_at_ns"],
                "remaining_wall_time_ns": remaining_ns,
                "native_entrypoint": native_entrypoint,
            },
        )

        self._save(directory / "run.json", state)

        command = self.compose + [
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
            self._container_path(attempt_directory / "attempt_input.json"),
        ]

        started_ns = time.perf_counter_ns()
        with (attempt_directory / "launch.log").open("w", encoding="utf-8") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            attempt["pid"] = process.pid
            self._save(directory / "run.json", state)
            try:
                code = process.wait(timeout=remaining_ns / 1_000_000_000)
                launch_result = {
                    "return_code": code,
                    "elapsed_wall_time_ns": time.perf_counter_ns() - started_ns,
                    "ended_at_ns": time.time_ns(),
                }
                self._save(attempt_directory / "launch_result.json", launch_result)
                container_state = self._container_state(container)
                failed_start = container_state is None or (
                    container_state["Status"] == "created" and bool(container_state["Error"])
                )
                if code != 0 and failed_start:
                    self._finish_attempt(
                        state, "failed", code, launch_result["elapsed_wall_time_ns"], launch_result["ended_at_ns"]
                    )
                else:
                    subprocess.run(["docker", "wait", container], check=True, stdout=log, stderr=subprocess.STDOUT)
                    self._reconcile(state)
            except subprocess.TimeoutExpired:
                self._stop(container, process, log)
                if self._container_state(container) is None:
                    self._finish_attempt(
                        state, "budget_exhausted", 124, time.perf_counter_ns() - started_ns, time.time_ns()
                    )
                else:
                    self._reconcile(state)
            except KeyboardInterrupt:
                self._stop(container, process, log)
                if self._container_state(container) is None:
                    self._finish_attempt(state, "interrupted", 130, time.perf_counter_ns() - started_ns, time.time_ns())
                else:
                    self._reconcile(state)
            finally:
                self._save(directory / "run.json", state)

        if state["status"] == "captured":
            self._finalize(directory, state)

    def _finalize(self, directory: Path, state: dict[str, Any]) -> None:
        if self._completed_processing(state):
            self._save(directory / "run.json", state)
            return

        self._raise_processing_failure(directory, state)

        if state["role"] == "performance":
            with (directory / "prepared_input.json").open(encoding="utf-8") as stream:
                prepared = json.load(stream)
            if prepared["measurement_kind"] == "cpu":
                raise RuntimeError("native CPU sampler exited without publishing its performance manifest")

        state["task_status"] = "processing"

        self._save(directory / "run.json", state)
        capture = Path(state["attempts"][-1]["directory"]) / "native_capture.json"
        try:
            with (capture.parent / "processing_launch.log").open("a", encoding="utf-8") as log:
                subprocess.run(
                    self.compose
                    + [
                        "run",
                        "--rm",
                        "-T",
                        "--no-deps",
                        "-e",
                        "PYTHONPATH=/workspace/trace-sim/src/collection",
                        "modeling",
                        "python3",
                        "-m",
                        "markov_collection.workflow.processing",
                        "--capture",
                        self._container_path(capture),
                        "--role",
                        state["role"],
                    ],
                    check=True,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                )
            if not self._completed_processing(state):
                raise RuntimeError("capture processor exited without publishing its result")
        except subprocess.CalledProcessError:
            self._raise_processing_failure(directory, state)
            state.update(task_status="interrupted", reason=f"{state['role']}_processing_interrupted")
            self._save(directory / "run.json", state)
            raise
        except (Exception, KeyboardInterrupt):
            state.update(task_status="interrupted", reason=f"{state['role']}_processing_interrupted")
            self._save(directory / "run.json", state)
            raise

        self._save(directory / "run.json", state)

    def _raise_processing_failure(self, directory: Path, state: dict[str, Any]) -> None:
        attempt = state["attempts"][-1]
        try:
            with (Path(attempt["directory"]) / "processing_failure.json").open(encoding="utf-8") as stream:
                failure = json.load(stream)
        except FileNotFoundError:
            return

        attempt["processing_failure"] = failure
        state.update(task_status="finished", reason=f"{state['role']}_processing_failed")
        self._save(directory / "run.json", state)
        raise CaptureProcessingFailed(failure["message"])

    def _completed_processing(self, state: dict[str, Any]) -> bool:
        role = state["role"]
        if state["status"] != "captured" or state[f"{role}_manifest"] is not None:
            return False

        directory = Path(state["attempts"][-1]["directory"])
        result_path = directory / ("performance_manifest.json" if role == "performance" else "processing_result.json")
        try:
            with result_path.open(encoding="utf-8") as stream:
                result = json.load(stream)
        except FileNotFoundError:
            return False

        if role == "base":
            state.update(base_manifest=str(directory / "profile_manifest.json"), source_readiness=result["readiness"])
        elif role == "validation":
            state.update(
                validation_manifest=str(directory / "validation_manifest.json"),
                observation_readiness=result["readiness"],
            )
        else:
            state.update(performance_manifest=str(result_path), sample_readiness=result["readiness"])
        state["task_status"] = "finished"
        state.pop("reason", None)

        return True

    def resume(self, directory: Path, *, budgets: dict[str, int] | None = None) -> dict[str, Any]:
        with (directory / "run.lock").open("a", encoding="utf-8") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                return {**self.status(directory), "workflow_busy": True}

            with (directory / "run.json").open(encoding="utf-8") as stream:
                state = json.load(stream)
            live, active_ns = self._reconcile(state)
            self._save(directory / "run.json", state)
            if live:
                result = {"run": str(directory), "live_attempt": True, "active_wall_time_ns": active_ns, **state}
                if budgets:
                    result["requested_changes_applied"] = False
                return result

            if budgets:
                self._revise_budget(directory, state, budgets)
            if state["status"] == "captured":
                if state[f"{state['role']}_manifest"] is None:
                    self._finalize(directory, state)
                return {"run": str(directory), **state}
            if not state["collection"][state["role"]]["allow_run"]:
                return {"run": str(directory), **state}

            # Reuse recorded inputs; changing business configuration requires a new run.
            with (directory / "prepared_input.json").open(encoding="utf-8") as stream:
                prepared = json.load(stream)
            self._collect(prepared, directory, state)

        return {"run": str(directory), **state}

    def _revise_budget(self, directory: Path, state: dict[str, Any], budgets: dict[str, int]) -> None:
        role = state["role"]
        if set(budgets) != {role}:
            raise ValueError(f"this {role} collection task only accepts its own budget")

        seconds = budgets[role]
        if seconds <= 0 or seconds * 1_000_000_000 < state["collection"][role]["consumed_wall_time_ns"]:
            raise ValueError(f"{role} total budget must be positive and cover its consumed time")
        with (directory / "prepared_input.json").open(encoding="utf-8") as stream:
            prepared = json.load(stream)
        if prepared["environment"] is None:
            raise ValueError("enabled collection requires the task's declared environment")

        permission = {"allow_run": True, "max_wall_time_sec": seconds}
        state["collection"][role].update(permission)

        self._save(directory / "run.json", state)

    def _container_state(self, container: str) -> dict[str, Any] | None:
        inspection = subprocess.run(
            ["docker", "inspect", "--format", "{{json .State}}", container],
            capture_output=True,
            text=True,
            check=False,
        )
        if inspection.returncode == 0:
            return json.loads(inspection.stdout)
        if "No such object" in inspection.stderr or "No such container" in inspection.stderr:
            return None

        raise subprocess.CalledProcessError(
            inspection.returncode, inspection.args, inspection.stdout, inspection.stderr
        )

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
        attempt = state["attempts"][-1]
        attempt.update(status=status, return_code=code, elapsed_wall_time_ns=elapsed_ns, ended_at_ns=ended_ns)

        role = state["role"]
        state["collection"][role]["consumed_wall_time_ns"] += elapsed_ns

        state.update(task_status="finished", status="needs_data")
        if status == "completed":
            state.update(task_status="processing", status="captured")
        elif status == "failed":
            state.update(status="failed", reason=f"{role}_collection_failed")
        elif status == "interrupted":
            state.update(task_status="interrupted", reason=f"{role}_collection_interrupted")
        else:
            state["reason"] = f"{role}_collection_budget_exhausted"

    def _reconcile(self, state: dict[str, Any]) -> tuple[bool, int]:
        if not state["attempts"] or state["attempts"][-1]["status"] != "collecting":
            return False, 0

        attempt = state["attempts"][-1]
        container = self._container_state(attempt["container"])
        try:
            with (Path(attempt["directory"]) / "attempt_status.json").open(encoding="utf-8") as stream:
                record = json.load(stream)
        except FileNotFoundError:
            record = None  # The container has not entered its monitor yet.

        if container is None or container["Status"] == "created":
            if self._live_launcher(attempt):
                return True, time.time_ns() - attempt["created_at_ns"]
            try:
                with (Path(attempt["directory"]) / "launch_result.json").open(encoding="utf-8") as stream:
                    launch = json.load(stream)
            except FileNotFoundError:
                launch = None

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

    def status(self, directory: Path) -> dict[str, Any]:
        with (directory / "run.json").open(encoding="utf-8") as stream:
            state = json.load(stream)
        live, active_ns = self._reconcile(state)
        self._completed_processing(state)
        return {"run": str(directory), "live_attempt": live, "active_wall_time_ns": active_ns, **state}

    def _stop(self, container: str, process: subprocess.Popen[Any], log: Any) -> None:
        if process.poll() is None:
            process.terminate()
        process.wait()
        if self._container_state(container) is not None:
            subprocess.run(
                ["docker", "stop", "--time", "30", container], stdout=log, stderr=subprocess.STDOUT, check=True
            )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["profile", "observe-target", "sample", "predict", "resume", "status"])
    parser.add_argument("--configuration", help="configuration id for explicit diagnostic acquisition")
    parser.add_argument("--workload", help="request workload id for explicit acquisition or sampling prompts")
    parser.add_argument("--budget-sec", type=int, help="explicit target observation wall-time budget")
    parser.add_argument("--validation-budget-sec", type=int, help="new total target observation budget on resume")
    parser.add_argument("--repetitions", type=int, default=3, help="independent sampling repetitions per grid point")
    parser.add_argument(
        "--measurement-requests",
        type=Path,
        help="sample explicit missing phase/input/context/prompt/finished-count work domains",
    )
    parser.add_argument(
        "--kind", choices=["cpu", "device", "communication"], default="cpu", help="independent measurement kind"
    )

    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--project", type=Path)
    inputs.add_argument("--run", type=Path)
    parser.add_argument("--template-assets", type=Path, help="replace prediction template imports with a JSON array")
    parser.add_argument(
        "--cost-model", type=Path, action="append", help="replace fitted cost inputs on prediction resume"
    )
    parser.add_argument(
        "--performance-manifest",
        type=Path,
        action="append",
        help="replace prediction performance imports; repeat for multiple independent assets",
    )
    parser.add_argument("--base-budget-sec", type=int, help="new total base budget for prediction resume")
    parser.add_argument("--performance-budget-sec", type=int, help="new total performance budget for prediction resume")
    parser.add_argument("--representative", action="store_true", help="observe the planned representative recipe")
    arguments = parser.parse_args()

    if arguments.representative and (arguments.command != "profile" or arguments.workload is not None):
        parser.error("--representative requires profile without --workload")

    acquisition = arguments.command in {"profile", "sample", "observe-target"}
    if acquisition:
        if arguments.configuration is None:
            parser.error("explicit acquisition requires --configuration")
        communication = arguments.command == "sample" and arguments.kind == "communication"
        if not communication and not arguments.representative and arguments.workload is None:
            parser.error("explicit acquisition requires --workload")
        if communication and arguments.workload is not None:
            parser.error("communication sampling uses declared device assets, not --workload")
    elif arguments.configuration is not None or arguments.workload is not None:
        parser.error("--configuration and --workload apply only to explicit acquisition")

    if arguments.command == "observe-target":
        if arguments.budget_sec is None or arguments.budget_sec <= 0:
            parser.error("observe-target requires a positive --budget-sec")
    elif arguments.budget_sec is not None:
        parser.error("--budget-sec requires observe-target")

    if arguments.measurement_requests is not None and (
        arguments.command != "sample" or arguments.kind == "communication"
    ):
        parser.error("measurement requests require sample --kind cpu|device")

    if arguments.command == "sample" and arguments.kind != "communication" and arguments.measurement_requests is None:
        parser.error("CPU/device sampling requires --measurement-requests; automatic workload grids are not supported")

    revisions = any(
        value is not None
        for value in [
            arguments.template_assets,
            arguments.performance_manifest,
            arguments.cost_model,
            arguments.base_budget_sec,
            arguments.performance_budget_sec,
            arguments.validation_budget_sec,
        ]
    )
    if revisions and arguments.command != "resume":
        parser.error("asset and budget revisions require resume")

    root = Path(__file__).resolve().parents[4]
    workflow = CollectionWorkflow(root)
    if arguments.command == "predict":
        if arguments.project is None:
            parser.error("predict requires --project")
        from markov_collection.workflow.prediction import PredictionWorkflow

        result = PredictionWorkflow(workflow).predict(arguments.project.resolve())
    elif arguments.command in {"profile", "sample", "observe-target"}:
        if arguments.project is None:
            parser.error(f"{arguments.command} requires --project")
        if arguments.repetitions <= 0:
            parser.error("sampling repetitions must be positive")
        result = workflow.profile(
            arguments.project.resolve(),
            role={"profile": "base", "sample": "performance", "observe-target": "validation"}[arguments.command],
            configuration_id=arguments.configuration,
            workload_id=arguments.workload,
            budget_sec=arguments.budget_sec,
            representative=arguments.representative,
            repetitions=arguments.repetitions,
            measurement_kind=arguments.kind,
            measurement_requests=arguments.measurement_requests.resolve()
            if arguments.measurement_requests is not None
            else None,
        )
    else:
        if arguments.run is None:
            parser.error(f"{arguments.command} requires --run")
        directory = arguments.run.resolve()
        with (directory / "run.json").open(encoding="utf-8") as stream:
            state = json.load(stream)
        budgets = {
            role: seconds
            for role, seconds in [
                ("base", arguments.base_budget_sec),
                ("performance", arguments.performance_budget_sec),
                ("validation", arguments.validation_budget_sec),
            ]
            if seconds is not None
        }
        if "prediction" in state:
            if arguments.validation_budget_sec is not None:
                parser.error("validation budget applies only to an observe-target task")
            from markov_collection.workflow.prediction import PredictionWorkflow

            prediction = PredictionWorkflow(workflow)
            result = (
                prediction.status(directory)
                if arguments.command == "status"
                else prediction.resume(
                    directory,
                    budgets=budgets,
                    template_assets=arguments.template_assets.resolve()
                    if arguments.template_assets is not None
                    else None,
                    performance_manifests=(
                        [path.resolve() for path in arguments.performance_manifest]
                        if arguments.performance_manifest is not None
                        else None
                    ),
                    cost_models=[path.resolve() for path in arguments.cost_model]
                    if arguments.cost_model is not None
                    else None,
                )
            )
        else:
            if (
                arguments.template_assets is not None
                or arguments.performance_manifest is not None
                or arguments.cost_model is not None
            ):
                parser.error("asset revisions require a prediction task")
            result = (
                workflow.status(directory)
                if arguments.command == "status"
                else workflow.resume(directory, budgets=budgets)
            )

    print(json.dumps(result, indent=2))
    if result.get("status") == "failed" and arguments.command != "status":
        raise SystemExit(1)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(error.stderr.strip() if isinstance(error.stderr, str) else str(error), file=sys.stderr)
        raise SystemExit(2) from None
    except (ValueError, OSError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(2) from None
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1) from None
