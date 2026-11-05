"""Run an independent investigation with a frozen core and reviewed adapter scopes."""

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

from markov_application.command import CommandFailed
from markov_application.environment import NativeDeviceUnavailable
from markov_application.exploration import BudgetExceeded, Exploration
from markov_application.specification import read_specification
from markov_collection.artifacts import write_json
from validation import NativeValidation, analyze, completion_checks


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
        super().step(command, category)

    def storage_bytes(self, retained: bool = False) -> int:
        self.investigation.check_storage(retained)
        return super().storage_bytes(retained)

    def release_exports(self, domains: dict[str, dict[str, Any]]) -> None:

        # Independent investigation retains SDK facts at their original paths for later reinterpretation.
        pass


class Investigation:
    def __init__(self, root: Path, directory: Path) -> None:
        self.root = root
        self.directory = directory
        self.manifest = json.loads((directory / "inputs/matrix.json").read_text())
        self.jobs = {job["id"]: job for job in self.manifest["jobs"]}
        self.case_tasks = {"workflow/" + job["id"]: job["task"] for job in self.jobs.values()}
        self.case_tasks.update(
            {self.native_key(job, case): job["task"] for job in self.jobs.values() for case in job["native_cases"]}
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
        tasks = {job["task"] for job in self.jobs.values()}
        tasks.update(case["task"] for case in self.manifest["lifecycle_cases"] if "task" in case)

        # Reviewed adapter updates can replace a task path; its recorded attempts remain charged.
        tasks.update(outcome["source_task"] for outcome in self.outcomes if outcome["source_task"] is not None)
        paths = {(self.directory / task).resolve().parent.parent / "state.json" for task in tasks}
        return [json.loads(path.read_text()) for path in sorted(paths) if path.is_file()]

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

    def verify_freeze(self, *, capture_only: bool = False) -> None:
        allowed = {"capture_frozen", "frozen"} if capture_only else {"frozen"}
        if self.manifest["status"] not in allowed:
            raise ValueError("freeze the reviewed code and inputs before starting the investigation")
        prefix = os.environ.get("MARKOV_IMAGE_PREFIX", "markov-next")
        names = [f"{prefix}-sglang:ubuntu22.04", f"{prefix}-control:ubuntu22.04", f"{prefix}-modeling:ubuntu24.04"]
        if capture_only:

            # Acquisition loads its package and SDK source from the fixed image, never the editable core.
            names = names[:1]
        expected = {names[0]: self.manifest["capture_image"]} if capture_only else self.manifest["images"]
        images = json.loads(subprocess.check_output(["docker", "image", "inspect", *names], text=True))
        if dict(zip(names, (image["Id"] for image in images), strict=True)) != {
            name: expected[name] for name in names
        }:
            raise ValueError("execution images changed after freezing")
        if capture_only:
            return

        paths = [
            "src",
            "example/automatic_graph",
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
            "src/collection/native",
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

    def done(self, case: str, *, capture_only: bool = False) -> bool:
        previous = next((item for item in reversed(self.outcomes) if item["case"] == case), None)
        incomplete = NativeValidation.resumable | {"blocked", "sources_complete", "templates_complete"}
        if capture_only:
            incomplete -= {"captured", "sources_complete", "templates_complete"}
        if (
            previous is None
            or previous["status"] in incomplete
            or (not capture_only and previous.get("stage") == "sources")
        ):
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

    def execute(self, case: str, action, *arguments, capture_only: bool = False) -> None:
        if self.done(case, capture_only=capture_only):
            return
        self.verify_freeze(capture_only=capture_only)
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

    def workflow(self, job: dict[str, Any], sources_only: bool = False) -> dict[str, Any]:
        directory = (self.directory / job["task"]).parent.parent
        with (directory / ".lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            task = InvestigationTask(self, job)
            task.run(stop_after="sources" if sources_only else "predictions")
        if sources_only:
            return {
                "status": task.state["status"], "stage": "sources", "source_sessions": len(task.state["sources"]),
                "report": str((task.directory / "results/summary.md").relative_to(self.directory)),
            }
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
        if not all(task.asset_path(domain).is_file() for domain in task.domain_settings):
            raise FileNotFoundError(f"source library was not published: {job_id}")
        return task

    @staticmethod
    def native_key(job: dict[str, Any], case: dict[str, Any]) -> str:
        key = f"native/{job['id']}/{case['configuration']}/{case['workload']}"
        return key + "/" + case["control"] if "control" in case else key

    def native(self, job: dict[str, Any], case: dict[str, Any], capture_only: bool = False) -> dict[str, Any]:
        directory = (self.directory / job["task"]).parent.parent
        with (directory / ".lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                task = InvestigationTask(self, job) if capture_only else self.task(job["id"])
            except FileNotFoundError as error:
                if capture_only:
                    raise
                return {"status": "blocked", "depends_on": "workflow/" + job["id"], "reason": str(error)}
            capture = task.directory / "captures" / f"validation-{case['configuration']}-{case['workload']}"
            try:
                task.environment.cleanup()
                validation = NativeValidation(
                    task,
                    case["configuration"],
                    case["workload"],
                    capture,
                    case.get("minimum_batch_members", 1),
                    case.get("control"),
                )
                previous = json.loads(validation.report.read_text()) if validation.report.exists() else None
                outcome = (
                    previous
                    if previous and previous["status"] not in NativeValidation.resumable
                    else validation.run(resume=True, capture_only=capture_only)
                )
                if not capture_only:
                    for subject, minimum in case.get("minimum_coverage", {}).items():
                        self.coverage(outcome, subject, minimum, outcome.get(subject + "_evidence", {}))
                outcome["report"] = str(validation.report.relative_to(self.directory))
                self.check_storage(retained=True)
                validation.release_observations({validation.control})
                return outcome
            finally:
                task.environment.cleanup()
                task.report()

    @staticmethod
    def coverage(outcome: dict, subject: str, minimum: dict, evidence: dict) -> None:
        observed = evidence.get("coverage", {}).get("observed", {})
        checks = {
            name: {"minimum": value, "observed": observed.get(name), "covered": observed.get(name, 0) >= value}
            for name, value in minimum.items()
        }
        outcome.setdefault("coverage", {})[subject] = checks
        if outcome["status"] in ("complete", "matched") and not all(item["covered"] for item in checks.values()):
            outcome["status"] = "partial"

    @staticmethod
    def completed(target: dict[str, Any], result: dict[str, Any]) -> bool:
        return all(completion_checks(target, result).values())

    @staticmethod
    def prediction(
        task: Exploration, target: Path, output: Path, expected: str, error_terms: list[str], *, control: str
    ) -> dict[str, Any]:
        asset = task.asset_path("domain-0")
        detailed = task.directory / "work/investigation" / output.parent.name / "prediction.json"
        try:
            task.simulate(asset, target, detailed, True, control=control)
        except CommandFailed as error:

            # A worker crash is not a successful negative case; require the declared input error.
            rejected = (
                expected == "rejected"
                and error.returncode == 1
                and bool(error_terms)
                and all(term in error.stderr for term in error_terms)
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
            return self.prediction(
                task, self.directory / case["input"], output, case["expected"], case["error_terms"],
                control=case["control"],
            )

    def public(
        self, arguments: list[str], output: Path, timeout: int = 30, owner: Exploration | None = None
    ) -> subprocess.CompletedProcess[str]:
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("a+") as log:
            start = log.tell()
            try:
                result = subprocess.run(
                    [sys.executable, "-m", "markov_application.exploration", *arguments],
                    cwd=self.root,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    timeout=timeout,
                )
            except subprocess.TimeoutExpired:
                if owner is not None:
                    owner.environment.cleanup()
                raise

            # Preserve the log history, but let this invocation own only its new output.
            log.seek(start)
            result.stdout = log.read()
            return result

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
        model = directory / "inputs/model.json"
        if not model.exists():
            shutil.copyfile(source.directory / "inputs/model.json", model)
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("a") as log:
            process = subprocess.Popen(
                [sys.executable, "-m", "markov_application.exploration", "--resume", str(directory)],
                cwd=self.root,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            stopped = False
            try:
                deadline = time.monotonic() + 30
                while process.poll() is None and time.monotonic() < deadline:

                    # Read commands in the same snapshot; transient containers may vanish before inspect.
                    commands = subprocess.check_output(
                        [
                            "docker",
                            "ps",
                            "--no-trunc",
                            "--format",
                            "{{.Command}}",
                            "--filter",
                            f"label={task.environment.label}",
                        ],
                        text=True,
                    ).splitlines()
                    if any(" simulate " in command for command in commands):
                        process.send_signal(signal.SIGINT)
                        stopped = True
                        break
                    time.sleep(0.05)
                if process.poll() is None and not stopped:
                    process.send_signal(signal.SIGINT)
                process.wait(timeout=30)
                released_after_interrupt = self.released(task)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                task.environment.cleanup()
        before = json.loads((directory / "state.json").read_text())
        interrupted = (
            stopped and before["status"] == "interrupted" and before["pending"] is None and released_after_interrupt
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
            "workers_released_after_interrupt": released_after_interrupt,
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
                else result.returncode != 0 and "ValueError" in result.stdout
            )
            return {"status": "complete" if passed else "differences", "returncode": result.returncode}
        if kind == "budget":
            path = self.directory / case["task"]
            task = Exploration(self.root, path.parent.parent, read_specification(path), False)
            result = self.public(["--resume", str(task.directory)], output, owner=task)
            state = json.loads((task.directory / "state.json").read_text())
            passed = (
                result.returncode != 0
                and "BudgetExceeded" in result.stdout
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
            control = case.get("control", task.specification["control"])
            target = self.directory / case["input"]
            original = target.read_bytes()
            asset = task.asset_path("domain-0")
            paths = [(target, target)]
            if control == "native":
                for role in ("core", "native"):
                    result = output.parent / role / "prediction.json"
                    source = result.with_suffix(".logs") / (role + ".log")
                    source.parent.mkdir(parents=True, exist_ok=True)
                    source.write_bytes(original)
                    paths.append((source, result))
            checks = []
            for source, result in paths:
                try:
                    task.simulate(asset, source, result, False, control=control)
                except CommandFailed as error:
                    passed = (
                        error.returncode == 1
                        and "must not overwrite an input" in error.stderr
                        and source.read_bytes() == original
                    )
                    checks.append({"passed": passed, "error": str(error)})
                else:
                    checks.append({"passed": False})
            return {
                "status": "complete" if all(check["passed"] for check in checks) else "differences",
                "checks": checks,
            }
        raise ValueError(f"unsupported lifecycle case: {kind}")

    def run(self, *, capture_only: bool = False, source_job: str | None = None) -> None:
        self.verify_freeze(capture_only=capture_only or source_job is not None)
        if source_job is not None:
            job = self.jobs[source_job]
            case = "workflow/" + source_job
            self.execute(case, self.workflow, job, True, capture_only=True)
            if not self.done(case, capture_only=True):
                raise SystemExit(2)
            return
        if capture_only:
            cases = []
            for job in self.manifest["jobs"]:
                for case in job["native_cases"]:
                    key = self.native_key(job, case)
                    cases.append(key)
                    self.execute(key, self.native, job, case, True, capture_only=True)
            pending = [case for case in cases if not self.done(case, capture_only=True)]
            print(json.dumps({"native_capture_queue_complete": not pending, "pending": pending}), flush=True)
            if pending:
                raise SystemExit(2)
            return

        for job in self.manifest["jobs"]:
            self.execute("workflow/" + job["id"], self.workflow, job)
            for case in job["native_cases"]:
                self.execute(self.native_key(job, case), self.native, job, case)
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
    collection = parser.add_mutually_exclusive_group()
    collection.add_argument(
        "--collect-targets", action="store_true", help="collect fixed native targets without comparison"
    )
    collection.add_argument(
        "--collect-sources", metavar="JOB", help="collect the planned representatives of one source job"
    )
    arguments = parser.parse_args()
    root = Path.cwd()
    directory = arguments.run.resolve()
    directory.relative_to(root / "data")
    with (directory / ".lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        Investigation(root, directory).run(capture_only=arguments.collect_targets, source_job=arguments.collect_sources)
