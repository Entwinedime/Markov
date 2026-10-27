"""Run the S8 investigation with a frozen core and reviewed adapter scopes."""

from __future__ import annotations

import argparse
import fcntl
import json
import os
import shutil
import signal
import stat
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import write_json
from markov_collection.workflow.exploration import BudgetExceeded, CommandFailed, Exploration, read_specification
from validation import NativeDeviceUnavailable, NativeValidation, analyze, completion_checks


class InvestigationTask(Exploration):
    def __init__(self, investigation: Investigation, job: dict[str, Any]) -> None:
        self.investigation = investigation
        self.job = job
        source = investigation.directory / job["task"]
        super().__init__(investigation.root, source.parent.parent, read_specification(source), False)

    def available(self, category: str) -> int:
        return min(super().available(category), self.investigation.available(category))

    def step(self, command: list[str], category: str) -> None:
        if category == "native":
            attempts = self.investigation.native_attempts()
            if self.state["native_attempts"] >= self.job["native_attempt_cap"]:
                raise BudgetExceeded("domain native attempt limit exhausted")
            if attempts >= self.investigation.manifest["round_budget"]["native_attempts"]:
                raise BudgetExceeded("investigation native attempt limit exhausted")
            self.check_devices()
        super().step(command, category)

    def check_devices(self) -> None:

        # Query before Engine starts; never compete with another task already using a selected card.
        script = """import subprocess, sys
for device in sys.argv[1:]:
    command = ['npu-smi', 'info', '-t', 'proc-mem', '-i', device, '-c', '0']
    status = subprocess.check_output(command, text=True, timeout=15)
    if status.strip() != 'No process in device.':
        raise SystemExit(f'Native device {device} is occupied or its occupancy is unknown: {status}')
print('Selected native devices are idle')
"""
        command = ["python3", "-c", script, *map(str, self.environment.devices)]
        try:
            self.step(
                [
                    "docker",
                    "exec",
                    str(self.environment.native),
                    *self.environment.limited(command, self.available("processing")),
                ],
                "processing",
            )
        except CommandFailed as error:
            raise NativeDeviceUnavailable(str(error)) from error

    def storage_bytes(self, retained: bool = False) -> int:
        self.investigation.check_storage(retained)
        return super().storage_bytes(retained)

    def acquire(self, name: str, domain: dict[str, Any]) -> None:
        super().acquire(name, domain)

        # Preserve source evidence before the default product workflow discards consumed SDK exports.
        for key, relative in self.state["sources"].items():
            if not key.startswith(name + "-source-"):
                continue
            for parent in self.acquisition.observation_directories(domain, self.directory / relative):
                for child in ("native", "runtime"):
                    source = parent / child
                    if source.exists():
                        archive = parent / "evidence"
                        archive.mkdir(exist_ok=True)
                        source.rename(archive / child)


class Investigation:
    def __init__(self, root: Path, directory: Path) -> None:
        self.root = root
        self.directory = directory
        self.manifest = json.loads((directory / "inputs/matrix.json").read_text())
        self.jobs = {job["id"]: job for job in self.manifest["jobs"]}
        self.case_tasks = {"workflow/" + job["id"]: job["task"] for job in self.jobs.values()}
        self.case_tasks.update(
            {
                f"native/{job['id']}/{case['configuration']}/{case['workload']}": job["task"]
                for job in self.jobs.values()
                for case in job["native_cases"]
            }
        )
        for kind in ("conditional", "lifecycle"):
            self.case_tasks.update(
                {
                    kind + "/" + case["id"]: self.jobs[case["job"]]["task"] if "job" in case else case.get("task")
                    for case in self.manifest[kind + "_cases"]
                }
            )
        self.output = directory / "results/outcomes.json"
        self.outcomes = json.loads(self.output.read_text()) if self.output.exists() else []

    def states(self) -> list[dict[str, Any]]:
        paths = list(self.directory.glob("runs/*/state.json")) + list(self.directory.glob("lifecycle/*/state.json"))
        return [json.loads(path.read_text()) for path in paths]

    def available(self, category: str) -> int:
        charged = sum(state["charged_ns"][category] for state in self.states())
        remaining = self.manifest["round_budget"][category + "_seconds"] * 1_000_000_000 - charged
        if remaining <= 2_000_000_000:
            raise BudgetExceeded(f"investigation {category} time budget exhausted")
        return remaining

    def native_attempts(self) -> int:
        return sum(state["native_attempts"] for state in self.states())

    def check_storage(self, retained: bool = False) -> None:
        total = 0
        for path in self.directory.rglob("*"):
            if retained and "work" in path.relative_to(self.directory).parts:
                continue
            try:
                record = path.stat(follow_symlinks=False)
            except FileNotFoundError:
                continue
            if stat.S_ISREG(record.st_mode):
                total += record.st_size
        limit = self.manifest["round_budget"]["retained_bytes" if retained else "temporary_bytes"]
        if total > limit:
            raise BudgetExceeded("investigation storage budget exhausted")

    def verify_freeze(self) -> None:
        if self.manifest["status"] != "frozen":
            raise ValueError("freeze the reviewed code and inputs before starting the investigation")
        paths = [
            "src",
            "example",
            "docker",
            "scripts",
            "third_party",
            "docs/design",
            "CMakeLists.txt",
            "pyproject.toml",
            ".clang-format",
        ]
        adapters = [
            "src/markov/adapters",
            "src/collection/markov_collection/adapters",
            "src/collection/adapters",
            "docker/images/base/sglang",
            "docker/images/sglang",
        ]

        # An adapter revision must already be reviewed; changing folders cannot waive core semantics.
        frozen = paths + [":(exclude)" + path for path in adapters]
        for revision, scope in (
            (self.manifest["frozen_revision"], frozen),
            (self.manifest["adapter_revision"], adapters),
        ):
            subprocess.run(
                ["git", "diff", "--exit-code", revision, "--", *scope],
                cwd=self.root,
                check=True,
                stdout=subprocess.DEVNULL,
            )
        if subprocess.check_output(
            ["git", "ls-files", "--others", "--exclude-standard", "--", *paths], cwd=self.root, text=True
        ).strip():
            raise ValueError("untracked code was added after freezing")
        prefix = os.environ.get("MARKOV_IMAGE_PREFIX", "markov-next")
        names = [f"{prefix}-sglang:ubuntu22.04", f"{prefix}-modeling:ubuntu24.04"]
        images = json.loads(subprocess.check_output(["docker", "image", "inspect", *names], text=True))
        if dict(zip(names, (image["Id"] for image in images), strict=True)) != self.manifest["images"]:
            raise ValueError("execution images changed after freezing")

    def done(self, case: str) -> bool:
        previous = next((item for item in reversed(self.outcomes) if item["case"] == case), None)
        if previous is None or previous["status"] in NativeValidation.resumable | {"blocked"}:
            return False
        return previous["source_task"] == self.case_tasks[case]

    def record(self, case: str, outcome: dict[str, Any]) -> None:
        if outcome["status"] == "blocked" and "depends_on" in outcome:
            dependency = next(item for item in reversed(self.outcomes) if item["case"] == outcome["depends_on"])
            outcome["dependency_status"] = dependency["status"]
        self.outcomes.append({**outcome, "case": case, "source_task": self.case_tasks[case]})
        write_json(self.output, self.outcomes, indent=2)
        latest = {item["case"]: item for item in self.outcomes}
        counts: dict[str, int] = {}
        for item in latest.values():
            counts[item["status"]] = counts.get(item["status"], 0) + 1
        states = self.states()
        lines = [
            "# 执行结构检查结果",
            "",
            "本页记录执行结果；调查完成不等于结构资格全部通过。",
            "",
            "| 状态 | 案例数 |",
            "| --- | ---: |",
        ]
        lines += [f"| {status} | {count} |" for status, count in sorted(counts.items())]
        lines += [
            "",
            f"实际native {sum(state['charged_ns']['native'] for state in states) / 1e9:.3f}s，"
            f"processing {sum(state['charged_ns']['processing'] for state in states) / 1e9:.3f}s；"
            f"原生尝试{self.native_attempts()}次。",
            "",
            "| 案例 | 结果 | 说明 |",
            "| --- | --- | --- |",
        ]
        for item in latest.values():
            description = (
                item.get("error", item.get("reason", item.get("report", ""))).replace("\n", " ").replace("|", "\\|")
            )
            lines.append(f"| {item['case']} | {item['status']} | {description[:240]} |")
        (self.output.parent / "summary.md").write_text("\n".join(lines) + "\n")
        print(json.dumps({"case": case, "status": outcome["status"]}), flush=True)

    def execute(self, case: str, action, *arguments) -> None:
        if self.done(case):
            return
        self.verify_freeze()
        try:
            outcome = action(*arguments)
        except KeyboardInterrupt:
            self.record(case, {"status": "interrupted"})
            raise
        except BudgetExceeded as error:
            self.record(case, {"status": "budget_exhausted", "error": str(error)})
        except NativeDeviceUnavailable as error:
            self.record(case, {"status": "resource_blocked", "error": str(error)})
        except Exception as error:
            self.record(case, {"status": "error", "error_type": type(error).__name__, "error": str(error)})
        else:
            self.record(case, outcome)

    def workflow(self, job: dict[str, Any]) -> dict[str, Any]:
        task = InvestigationTask(self, job)
        with (task.directory / ".lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            task.run()
        invalid = []
        for key, relative in task.state["results"].items():
            target = json.loads((task.directory / "inputs/targets" / (key + ".json")).read_text())
            result = json.loads((task.directory / relative).read_text())
            if not self.completed(target, result):
                invalid.append(key)
        return {
            "status": "differences" if invalid else task.state["status"],
            "predictions": len(task.state["results"]),
            "invalid_completions": invalid,
            "prediction_errors": task.state["failures"],
            "report": str((task.directory / "results/summary.md").relative_to(self.directory)),
        }

    def task(self, job_id: str) -> InvestigationTask:
        task = InvestigationTask(self, self.jobs[job_id])
        if not all(
            (task.directory / "assets" / domain / "templates.json").is_file() for domain in task.domain_settings
        ):
            raise FileNotFoundError(f"source library was not published: {job_id}")
        return task

    def native(self, job: dict[str, Any], case: dict[str, Any]) -> dict[str, Any]:
        try:
            task = self.task(job["id"])
        except FileNotFoundError as error:
            return {"status": "blocked", "depends_on": "workflow/" + job["id"], "reason": str(error)}
        capture = task.directory / "captures" / f"validation-{case['configuration']}-{case['workload']}"
        with (task.directory / ".lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                validation = NativeValidation(
                    task, case["configuration"], case["workload"], capture, case.get("minimum_batch_members", 1)
                )
                previous = json.loads(validation.report.read_text()) if validation.report.exists() else None
                outcome = (
                    previous
                    if previous and previous["status"] not in NativeValidation.resumable
                    else validation.run(resume=True)
                )
                outcome["report"] = str(validation.report.relative_to(self.directory))
                self.check_storage(retained=True)
                return outcome
            finally:
                task.environment.cleanup()
                task.report()

    @staticmethod
    def completed(target: dict[str, Any], result: dict[str, Any]) -> bool:
        return all(completion_checks(target, result).values())

    @staticmethod
    def prediction(task: Exploration, target: Path, output: Path, expected: str) -> dict[str, Any]:
        asset = task.directory / "assets/domain-0/templates.json"
        detailed = task.directory / "work/investigation" / output.parent.name / "prediction.json"
        arguments = [
            "simulate",
            "--templates",
            str(asset),
            "--input",
            str(target),
            "--output",
            str(detailed),
            "--details",
        ]
        try:
            task.step(
                task.environment.tool_command(arguments, [asset, target], detailed, task.available("processing")),
                "processing",
            )
        except CommandFailed as error:
            rejected = (
                expected == "rejected"
                and error.returncode == 1
                and error.stderr.startswith("markov: ")
                and not detailed.exists()
            )
            return {
                "status": "rejected" if rejected else "error",
                "error_type": type(error).__name__,
                "error": str(error),
            }
        result = json.loads(detailed.read_text())
        completion = completion_checks(json.loads(target.read_text()), result)
        del result
        analyze(task, "evidence.py", ["--input", str(target), "--prediction", str(detailed), "--output", str(output)])
        evidence = json.loads(output.read_text())
        detailed.unlink()
        return {
            "status": "complete"
            if expected == "complete" and all(completion.values()) and evidence["status"] == "matched"
            else "differences",
            "completion": completion,
            "evidence": str(output.relative_to(task.root)),
        }

    def conditional(self, case: dict[str, Any]) -> dict[str, Any]:
        try:
            task = self.task(case["job"])
        except FileNotFoundError as error:
            return {"status": "blocked", "depends_on": "workflow/" + case["job"], "reason": str(error)}
        output = self.directory / "cases" / case["id"] / "result.json"
        with (task.directory / ".lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return self.prediction(task, self.directory / case["input"], output, case["expected"])

    def public(
        self, arguments: list[str], output: Path, timeout: int = 30, owner: Exploration | None = None
    ) -> subprocess.CompletedProcess[str]:
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("a") as log:
            try:
                return subprocess.run(
                    [sys.executable, "-m", "markov_collection.workflow.exploration", *arguments],
                    cwd=self.root,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    timeout=timeout,
                )
            except subprocess.TimeoutExpired:
                if owner is not None:
                    owner.environment.cleanup()
                raise

    def released(self, task: Exploration) -> bool:
        return not subprocess.check_output(
            ["docker", "ps", "-aq", "--filter", f"label={task.environment.label}"], text=True
        ).strip()

    def interrupt_resume(self, case: dict[str, Any], source: InvestigationTask, output: Path) -> dict[str, Any]:
        path = self.directory / case["task"]
        directory = path.parent.parent
        task = Exploration(self.root, directory, read_specification(path), False)
        if self.available("processing") < task.specification["budget"]["processing_seconds"] * 1_000_000_000:
            raise BudgetExceeded("insufficient round processing allocation for the lifecycle experiment")
        assets = directory / "assets"
        assets.mkdir(exist_ok=True)
        if not (assets / "domain-0").exists():
            (assets / "domain-0").symlink_to(source.directory / "assets/domain-0", target_is_directory=True)
        environment = directory / "inputs/environment.json"
        if not environment.exists():
            shutil.copyfile(source.directory / "inputs/environment.json", environment)
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("a") as log:
            process = subprocess.Popen(
                [sys.executable, "-m", "markov_collection.workflow.exploration", "--resume", str(directory)],
                cwd=self.root,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            stopped = False
            try:
                deadline = time.monotonic() + 30
                while process.poll() is None and time.monotonic() < deadline:
                    identities = subprocess.check_output(
                        ["docker", "ps", "-q", "--filter", f"label={task.environment.label}"], text=True
                    ).split()
                    if identities:
                        containers = json.loads(subprocess.check_output(["docker", "inspect", *identities], text=True))
                        if any("simulate" in container["Config"]["Cmd"] for container in containers):
                            process.send_signal(signal.SIGINT)
                            stopped = True
                            break
                    time.sleep(0.05)
                if process.poll() is None and not stopped:
                    process.send_signal(signal.SIGINT)
                process.wait(timeout=30)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                task.environment.cleanup()
        before = json.loads((directory / "state.json").read_text())
        interrupted = (
            stopped and before["status"] == "interrupted" and before["pending"] is None and self.released(task)
        )
        result = self.public(
            ["--resume", str(directory)],
            output,
            timeout=task.specification["budget"]["processing_seconds"] + 30,
            owner=task,
        )
        after = json.loads((directory / "state.json").read_text())
        completed = (
            result.returncode == 0
            and after["status"] == "complete"
            and after["native_attempts"] == 0
            and self.released(task)
        )
        monotonic = after["charged_ns"]["processing"] >= before["charged_ns"]["processing"]
        return {
            "status": "complete" if interrupted and completed and monotonic else "differences",
            "interrupted_state": before,
            "resumed_state": after,
        }

    def lifecycle(self, case: dict[str, Any]) -> dict[str, Any]:
        kind = case["kind"]
        output = self.directory / "cases" / case["id"] / "run.log"
        if kind in ("help", "reject-task"):
            arguments = ["--help"] if kind == "help" else ["--input", str(self.directory / case["input"])]
            result = self.public(arguments, output)
            passed = (
                result.returncode == 0
                if kind == "help"
                else result.returncode != 0 and "ValueError" in output.read_text()
            )
            return {"status": "complete" if passed else "differences", "returncode": result.returncode}
        if kind == "budget":
            path = self.directory / case["task"]
            task = Exploration(self.root, path.parent.parent, read_specification(path), False)
            result = self.public(["--resume", str(task.directory)], output, owner=task)
            state = json.loads((task.directory / "state.json").read_text())
            passed = (
                result.returncode != 0
                and "BudgetExceeded" in output.read_text()
                and state["pending"] is None
                and state["native_attempts"] == 0
                and self.released(task)
            )
            return {"status": "complete" if passed else "differences", "state": state, "returncode": result.returncode}
        try:
            task = self.task(case["job"])
        except FileNotFoundError as error:
            return {"status": "blocked", "depends_on": "workflow/" + case["job"], "reason": str(error)}
        if kind == "resume-completed":
            expected = len(task.specification["configurations"]) * len(task.specification["workloads"])
            if task.state["status"] != "complete" or len(task.state["results"]) != expected:
                return {
                    "status": "blocked",
                    "depends_on": "workflow/" + case["job"],
                    "reason": "the formal task did not complete",
                }
            before = task.state
            result = self.public(["--resume", str(task.directory)], output, owner=task)
            after = json.loads((task.directory / "state.json").read_text())
            return {
                "status": "complete" if result.returncode == 0 and before == after else "differences",
                "returncode": result.returncode,
            }
        if kind == "interrupt":
            return self.interrupt_resume(case, task, output)
        if kind == "missing-input":
            missing = task.directory / "work/absent-lifecycle-input.json"
            result = task.directory / "work/missing-input-result/result.json"
            try:
                task.environment.tool_command(["inspect"], [missing], result, task.available("processing"))
            except FileNotFoundError:
                return {"status": "complete" if not missing.exists() and not result.parent.exists() else "differences"}
            return {"status": "differences"}
        if kind == "protect-input":
            target = self.directory / case["input"]
            original = target.read_bytes()
            asset = task.directory / "assets/domain-0/templates.json"
            arguments = ["simulate", "--templates", str(asset), "--input", str(target), "--output", str(target)]
            try:
                task.step(
                    task.environment.tool_command(arguments, [asset, target], target, task.available("processing")),
                    "processing",
                )
            except CommandFailed as error:
                passed = (
                    error.returncode == 1 and error.stderr.startswith("markov: ") and target.read_bytes() == original
                )
                return {"status": "complete" if passed else "differences", "error": str(error)}
            return {"status": "differences"}
        raise ValueError(f"unsupported lifecycle case: {kind}")

    def run(self) -> None:
        self.verify_freeze()
        for job in self.manifest["jobs"]:
            self.execute("workflow/" + job["id"], self.workflow, job)
        for job in self.manifest["jobs"]:
            for case in job["native_cases"]:
                self.execute(f"native/{job['id']}/{case['configuration']}/{case['workload']}", self.native, job, case)
        for case in self.manifest["conditional_cases"]:
            self.execute("conditional/" + case["id"], self.conditional, case)
        for case in self.manifest["lifecycle_cases"]:
            self.execute("lifecycle/" + case["id"], self.lifecycle, case)
        pending = [case for case in self.case_tasks if not self.done(case)]
        print(json.dumps({"investigation_complete": not pending, "pending": pending}), flush=True)
        if pending:
            raise SystemExit(2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True)
    arguments = parser.parse_args()
    root = Path.cwd()
    directory = arguments.run.resolve()
    directory.relative_to(root / "data")
    with (directory / ".lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        Investigation(root, directory).run()
