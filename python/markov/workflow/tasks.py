"""Persistent local task handles; modeling workflows still own all hardware and budgets."""

from __future__ import annotations

from contextlib import contextmanager
import fcntl
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import uuid

from markov.artifacts.request import PredictionRequest, read_json
from markov.artifacts.writing import write_json
from markov.workflow.planning import plan_prediction
from markov.workflow.search import SearchPolicy


ACTIONS = {"predict", "search", "maintain"}
FINISHED = {"completed", "incomplete", "failed", "stopped", "interrupted"}


def result_summary(result: dict) -> dict:
    if "cells" not in result:
        return result
    return {
        **result,
        "cells": [
            {key: value for key, value in cell.items() if key not in ("requests", "execution_graph")}
            for cell in result["cells"]
        ],
    }


def process_identity(pid: int) -> str | None:
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return None if fields[0] == "Z" else fields[19]
    except FileNotFoundError:
        return None


@contextmanager
def task_lock(path: Path, blocking: bool = False):
    with path.open("a") as stream:
        fcntl.flock(stream, fcntl.LOCK_EX | (0 if blocking else fcntl.LOCK_NB))
        yield


class Tasks:
    def __init__(self, directory: Path):
        self.directory = directory.resolve()
        self.directory.mkdir(parents=True, exist_ok=True)

    def path(self, identity: str) -> Path:
        if not re.fullmatch(r"[0-9a-f]{32}", identity):
            raise ValueError("Invalid task identifier")
        path = self.directory / identity
        if not path.is_dir():
            raise ValueError("Task does not exist")
        return path

    def state(self, identity: str) -> dict:
        path = self.path(identity) / "task.json"
        row = read_json(path)
        row["alive"] = row.get("pid") is not None and process_identity(row["pid"]) == row.get(
            "process_identity"
        )
        if not row["alive"] and row["status"] not in FINISHED:
            row["status"] = "interrupted"
        if row["alive"] and (path.parent / "stop.requested").exists():
            row["status"] = "stopping"
        return row

    def list(self) -> list[dict]:
        rows = [
            self.state(path.name)
            for path in self.directory.iterdir()
            if path.is_dir() and (path / "task.json").is_file()
        ]
        return sorted(rows, key=lambda row: row["created_at"], reverse=True)

    def create(self, action: str, request: dict, title: str, policy: dict | None = None) -> dict:
        if action not in ACTIONS:
            raise ValueError("Choose predict, search or maintain")
        if not isinstance(title, str) or not title.strip():
            raise ValueError("Give the task a name")
        if action != "maintain":
            parsed = PredictionRequest.parse(request, self.directory)
            plan_prediction(parsed)
            if action == "search":
                parsed_policy = SearchPolicy.parse({**policy, "prediction": "prediction.json"})
                if parsed_policy.baseline not in {target.name for target in parsed.targets}:
                    raise ValueError("Search baseline must name a target")
            request = parsed.document()

        with task_lock(self.directory / "tasks.lock"):
            if any(row["alive"] for row in self.list()):
                raise ValueError("Another panel task is active; stop it or wait before starting this task")
            identity = uuid.uuid4().hex
            directory = self.directory / identity
            directory.mkdir()
            write_json(directory / "request.json", request)
            if action == "search":
                write_json(
                    directory / "search.json", {**policy, "prediction": str(directory / "request.json")}
                )
            write_json(
                directory / "task.json",
                {
                    "id": identity,
                    "title": title.strip(),
                    "action": action,
                    "status": "ready",
                    "created_at": time.time(),
                    "request": str(directory / ("search.json" if action == "search" else "request.json")),
                },
            )
            return self._launch(identity)

    def resume(self, identity: str) -> dict:
        with task_lock(self.directory / "tasks.lock"):
            current = self.state(identity)
            if current["status"] == "completed":
                raise ValueError("Completed tasks can be reused, not resumed")
            if any(row["alive"] for row in self.list()):
                raise ValueError("A panel task is still active")
            return self._launch(identity)

    def _launch(self, identity: str) -> dict:
        directory = self.path(identity)
        with task_lock(directory / "run.lock"):
            (directory / "stop.requested").unlink(missing_ok=True)
            row = read_json(directory / "task.json")
            with (directory / "worker.log").open("a") as log:
                process = subprocess.Popen(
                    [sys.executable, "-m", "markov.workflow.tasks", str(self.directory), identity],
                    stdin=subprocess.DEVNULL,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
            row.update(
                status="starting",
                pid=process.pid,
                process_identity=process_identity(process.pid),
                started_at=time.time(),
            )
            row.pop("error", None)
            row.pop("finished_at", None)
            row.pop("result", None)
            row.pop("summary", None)
            write_json(directory / "task.json", row)
        return self.state(identity)

    def stop(self, identity: str) -> dict:
        row = self.state(identity)
        if row["alive"]:
            try:
                with (self.path(identity) / "stop.requested").open("x") as stream:
                    stream.write(str(time.time()))
            except FileExistsError:
                return row
            try:
                os.kill(row["pid"], signal.SIGINT)
            except ProcessLookupError:
                pass
        return self.state(identity)

    def result_path(self, identity: str) -> Path | None:
        row = self.state(identity)
        if row.get("result") is None:
            return None
        return Path(row["result"])

    def detail(self, identity: str) -> dict:
        row = self.state(identity)
        directory = self.path(identity)
        request = read_json(directory / "request.json")
        row["document"] = request
        if row["action"] == "search":
            row["policy"] = read_json(directory / "search.json")
        output = Path(request["execution"]["output"]) if row["action"] != "maintain" else None
        row["progress"] = {}
        row["budgets"] = []
        if output is not None:
            progress = output / "workflow_state.json"
            if progress.exists():
                row["progress"] = read_json(progress)
            for label, relative, limit in (
                ("base", "base", request["execution"].get("capture", {}).get("budget")),
                (
                    "performance",
                    "performance/sampling",
                    request.get("performance", {}).get("sampling_budget"),
                ),
            ):
                state = output / relative / "capture_state.json"
                if state.exists():
                    row["budgets"].append({"kind": label, "limit": limit, "state": read_json(state)})
            log = output / "prediction/run.log"
            if log.exists():
                with log.open("rb") as stream:
                    stream.seek(max(0, log.stat().st_size - 8192))
                    row["runtime_log"] = stream.read().decode(errors="replace")
        log = directory / "worker.log"
        if log.exists():
            with log.open("rb") as stream:
                stream.seek(max(0, log.stat().st_size - 8192))
                row["worker_log"] = stream.read().decode(errors="replace")
        summary = Path(row["summary"]) if row.get("result") is not None else None
        row["result"] = read_json(summary) if summary is not None else None
        return row


def run_task(directory: Path, identity: str) -> None:
    tasks = Tasks(directory)
    output = tasks.path(identity)
    with task_lock(output / "run.lock", blocking=True):
        row = read_json(output / "task.json")
        row["status"] = "running"
        write_json(output / "task.json", row)
        try:
            if row["action"] == "predict":
                from markov.workflow.prediction import predict

                result = predict(PredictionRequest.load(Path(row["request"])))
            elif row["action"] == "search":
                from markov.workflow.search import search

                result = search(Path(row["request"]))
            else:
                from markov.workflow.performance import maintain_performance

                result = maintain_performance(Path(row["request"]))
            path = Path(result["report"]) if "report" in result else output / "result.json"
            if "report" not in result:
                write_json(path, result)
            summary = output / "result_summary.json" if "cells" in result else path
            if summary != path:
                write_json(summary, result_summary(result))
            row.update(status=result["status"], result=str(path), summary=str(summary))
        except KeyboardInterrupt:
            row["status"] = "stopped"
        except Exception as error:
            row.update(status="failed", error=f"{type(error).__name__}: {error}")
        finally:
            row["finished_at"] = time.time()
            write_json(output / "task.json", row)


if __name__ == "__main__":
    run_task(Path(sys.argv[1]), sys.argv[2])
