"""Run the declared contract scenes, then measure their fixed same-budget endpoints."""

from __future__ import annotations

import argparse
import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

from markov.artifacts.request import PredictionRequest, read_json
from markov.artifacts.writing import write_json
from markov.workflow.capture import InferenceCapture
from markov.instrumentation.containers import container_state

from confirm import Confirmation
from run import candidates


class ContractSearch:
    def __init__(self, settings: Path, workloads: Path, models: Path, output: Path):
        self.settings = read_json(settings)
        self.workloads, self.models, self.output = workloads.resolve(), models.resolve(), output.resolve()
        self.scenes = list(read_json(self.workloads / "summary.json")["scenes"])
        self.targets = candidates(self.settings)
        self.output.mkdir(parents=True, exist_ok=True)

    def prepare(self) -> dict:
        for scene in self.scenes:
            directory = self.output / "scenes" / scene / "requests"
            directory.mkdir(parents=True, exist_ok=True)
            request = {
                "deployment": self.settings["deployment"],
                "base": {"configuration": self.settings["base_configuration"]},
                "workload": str(self.workloads / scene / "workload.json"),
                "targets": self.targets,
                "performance": {"models": str(self.models)},
                "execution": {
                    "output": str(directory.parent / "run"),
                    "kernel_cache_initial_state": "empty",
                    "memory_limit_gib": self.settings["prediction_memory_limit_gib"],
                    "capture": {**self.settings["capture"], "budget": self.settings["source_budget"]},
                },
            }
            path = directory / "prediction.json"
            if path.exists() and read_json(path) != request:
                raise ValueError("The scene already has different inputs; use a new output")
            write_json(path, request)
            PredictionRequest.load(path)
            write_json(directory / "search.json", {"prediction": str(path), **self.settings["policy"]})
        return {
            "status": "planned",
            "scenes": self.scenes,
            "candidates_per_scene": len(self.targets),
            "normal_roles": ["best_within_budget", "worst_within_budget"],
            "models": str(self.models),
        }

    def predict(self) -> None:
        running = []
        try:
            for scene in self.scenes:
                directory = self.output / "scenes" / scene
                report = directory / "run/search/report.json"
                if report.exists() and read_json(report)["status"] == "completed":
                    continue
                if state := container_state(directory / "run/prediction/container.id"):
                    if state["Running"]:
                        raise ValueError(
                            f"The recorded CPU prediction is still running: {scene}; wait for its existing handle"
                        )
                path = directory / "requests/prediction.json"
                request = PredictionRequest.load(path)
                memory = {
                    row.split(":")[0]: int(row.split()[1]) * 1024
                    for row in Path("/proc/meminfo").read_text().splitlines()
                }
                reserved = (
                    self.settings["capture"]["memory_limit_gib"] + 2 * self.settings["prediction_memory_limit_gib"]
                ) * 1024**3
                if memory["MemAvailable"] < reserved + 0.2 * memory["MemTotal"]:
                    raise ValueError(
                        "Insufficient host memory for the declared jobs and 20% headroom; no new capture started"
                    )
                captured = InferenceCapture(request).run(reuse=True)
                if captured["status"] not in ("completed", "reused"):
                    raise RuntimeError(f"Base capture did not complete: {scene}: {captured}")
                while len(running) >= 2:
                    self.finish(running[0])
                    running.pop(0)
                with (directory / "search.log").open("a") as stream:
                    process = subprocess.Popen(
                        [
                            sys.executable,
                            "-m",
                            "markov",
                            "search",
                            "--request",
                            str(directory / "requests/search.json"),
                        ],
                        stdout=stream,
                        stderr=subprocess.STDOUT,
                        start_new_session=True,
                    )
                running.append((scene, process))
                write_json(
                    directory / "cpu_process.json",
                    {
                        "pid": process.pid,
                        "started_at_sec": time.time(),
                        "search_request": str(directory / "requests/search.json"),
                    },
                )
            for entry in running:
                self.finish(entry)
            running.clear()
        finally:
            for scene, process in running:
                cid = self.output / "scenes" / scene / "run/prediction/container.id"
                state = container_state(cid)
                if state is not None and state["Running"]:
                    subprocess.run(["docker", "rm", "-f", cid.read_text().strip()], check=True, timeout=30)
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=10)

    def finish(self, entry) -> None:
        scene, process = entry
        if process.wait() != 0:
            raise RuntimeError(f"Search failed for {scene}; its log and formal report are retained")
        report = read_json(self.output / "scenes" / scene / "run/search/report.json")
        if report["status"] != "completed" or report["expected"] != len(self.targets):
            raise ValueError(f"Full declared coverage is incomplete: {scene}")

    def confirmation_plan(self) -> Path:
        path = self.output / "confirmation_plan.json"
        if path.exists():
            return path
        configurations = {target["name"]: target["configuration"] for target in self.targets}
        cases = []
        for position, scene in enumerate(self.scenes):
            directory = self.output / "scenes" / scene
            result = read_json(directory / "run/search/report.json")
            if result["status"] != "completed" or result["expected"] != len(self.targets):
                raise ValueError("Complete every declared prediction before selecting normal conditions")
            roles = {"best": result["recommended"], "worst": result["worst_within_budget"]}
            rows = {row["name"]: row for row in result["candidates"]}
            names = list(dict.fromkeys(roles.values()))
            if position % 2:
                names.reverse()
            raw = read_json(directory / "requests/prediction.json")
            for name in names:
                if name is None:
                    raise ValueError(f"No legal same-budget endpoint: {scene}")
                normal = {
                    **raw,
                    "base": {"configuration": configurations[name]},
                    "targets": [{"name": name, "configuration": {}}],
                    "execution": {
                        **raw["execution"],
                        "output": str(self.output / "normal" / scene / name),
                        "capture": {**self.settings["capture"], "budget": self.settings["normal_budget"]},
                    },
                }
                request = directory / "requests" / f"normal_{name}.json"
                write_json(request, normal)
                cases.append(
                    {
                        "scene": scene,
                        "target": name,
                        "roles": [role for role, target in roles.items() if target == name],
                        "request": str(request),
                        "predicted_sec": rows[name]["predicted_sec"],
                        "hourly_cost_units": rows[name]["hourly_cost_units"],
                        "applicable_limits_sec": [],
                    }
                )
        write_json(
            path,
            {
                "cases": cases,
                "comparison_reference": "worst",
                "initial_repetitions": 1,
                "maximum_repetitions": 1,
                "maximum_extra_conditions": 0,
            },
        )
        return path

    def report(self) -> dict:
        normal = read_json(self.output / "confirmation/results.json")
        scenes = {}
        for scene in self.scenes:
            search = read_json(self.output / "scenes" / scene / "run/search/report.json")
            rows = [row for row in normal["rows"] if row["scene"] == scene]
            best = next(row for row in rows if "best" in row["roles"])
            worst = next(row for row in rows if "worst" in row["roles"])
            better, worse = best["normal"]["mean_sec"], worst["normal"]["mean_sec"]
            scenes[scene] = {
                "search": str(self.output / "scenes" / scene / "run/search/report.json"),
                "expected": search["expected"],
                "statuses": search["statuses"],
                "best": best,
                "worst": worst,
                "actual_improvement_percent": 100 * (1 - better / worse)
                if better is not None and worse is not None
                else None,
            }
        result = {
            "status": normal["status"],
            "scenes": scenes,
            "models": str(self.models),
            "qualification": "Full declared package/policy scan; single fixed normal observation per endpoint. No target trace or refitting. Software transfer-admission quotas; no significance or real global optimum claim.",
        }
        write_json(self.output / "results.json", result)
        return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--settings", type=Path, required=True)
    parser.add_argument("--workloads", type=Path, required=True)
    parser.add_argument("--models", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--phase", choices=("plan", "predict", "confirm", "report", "all"), default="all")
    args = parser.parse_args()
    experiment = ContractSearch(args.settings, args.workloads, args.models, args.output)
    with (experiment.output / "experiment.lock").open("a") as lease:
        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if args.phase in ("plan", "predict", "all"):
            print(json.dumps(experiment.prepare(), ensure_ascii=False), flush=True)
        if args.phase in ("predict", "all"):
            experiment.predict()
        if args.phase in ("confirm", "all"):
            confirmation = Confirmation(experiment.confirmation_plan(), experiment.output / "confirmation")
            if not confirmation.measure():
                raise RuntimeError("Normal confirmation is incomplete; all observations and failures are retained")
            confirmation.report()
        if args.phase in ("report", "all"):
            print(json.dumps(experiment.report(), ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
