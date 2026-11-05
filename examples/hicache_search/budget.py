"""Budgeted HiCache selection over fully declared candidates and one maintained cost selection."""

from __future__ import annotations

from dataclasses import asdict, replace
import json
from pathlib import Path
import random
import time

from markov.artifacts.request import BaseInput, PredictionRequest, read_json, resolve_path
from markov.artifacts.writing import write_json
from markov.workflow.capture import InferenceCapture
from markov.workflow.performance import select_models
from markov.workflow.prediction import predict
from markov.workflow.search import SearchPolicy


BATCH_EVALUATIONS = 48


def features(configuration: dict) -> dict:
    extra = json.loads(configuration["hicache_storage_backend_extra_config"])
    return {
        "capacity": configuration["max_total_tokens"],
        "host": configuration["hicache_ratio"],
        "page": configuration["page_size"],
        "write": configuration["hicache_write_policy"],
        "threshold": extra["prefetch_threshold"],
        "wait": (configuration["hicache_storage_prefetch_policy"], extra.get("prefetch_timeout_base")),
    }


class BudgetSearch:
    """Use only requested predictions; reference enumeration and normal data are not inputs."""

    def __init__(self, path: Path, method: str, budgets: list[int], seed: int) -> None:
        raw = read_json(path)
        self.policy = SearchPolicy.parse(raw)
        self.request = PredictionRequest.load(resolve_path(path.resolve().parent, raw["prediction"]))
        self.targets = {target.name: target for target in self.request.targets}
        if self.policy.baseline not in self.targets:
            raise ValueError("Search baseline must name a supplied target")
        if not budgets or any(value <= 0 for value in budgets):
            raise ValueError("Prediction budgets must be positive")
        self.budgets = sorted({min(value, len(self.targets)) for value in budgets})
        self.method = method
        self.seed = seed
        self.rng = random.Random(seed)
        self.order = list(self.targets)
        self.rng.shuffle(self.order)
        self.priority = {name: index for index, name in enumerate(self.order)}
        self.features = {name: features(target.configuration) for name, target in self.targets.items()}
        self.cells: dict[str, dict] = {}
        self.output = self.request.execution.output / "search"
        self.output.mkdir(parents=True, exist_ok=True)
        self.state_path = self.output / "trials.json"
        self.state = (
            read_json(self.state_path)
            if self.state_path.exists()
            else {"method": method, "seed": seed, "budgets": self.budgets, "rounds": [], "checkpoints": []}
        )
        if (self.state["method"], self.state["seed"], self.state["budgets"]) != (method, seed, self.budgets):
            raise ValueError("This search output belongs to different method, seed or budgets; use a new output")

    def rank(self) -> dict:
        return self.policy.rank({"deployment": asdict(self.request.deployment), "cells": list(self.cells.values())})

    def choose(self, count: int) -> list[str]:
        remaining = [name for name in self.order if name not in self.cells]
        chosen = []
        if not self.cells:
            chosen.append(self.policy.baseline)
            remaining.remove(self.policy.baseline)
        if self.method == "random":
            return chosen + remaining[: count - len(chosen)]

        # Cover capacity/write/wait groups before refining observed promising points.
        def group(name: str) -> tuple:
            point = self.features[name]
            return point["capacity"], point["write"], point["wait"]

        covered = {group(name) for name in [*self.cells, *chosen]}
        groups: dict[tuple, list[str]] = {}
        for name in remaining:
            if group(name) not in covered:
                groups.setdefault(group(name), []).append(name)
        keys = list(groups)
        self.rng.shuffle(keys)
        for key in keys[: count - len(chosen)]:
            name = self.rng.choice(groups[key])
            chosen.append(name)
            remaining.remove(name)

        slots = count - len(chosen)
        if not slots:
            return chosen
        if not self.cells:
            return chosen + remaining[:slots]

        # Reserve a quarter of the remaining batch for uniform exploration.
        explore = self.rng.sample(remaining, min((slots + 3) // 4, len(remaining)))
        chosen.extend(explore)
        remaining = [name for name in remaining if name not in explore]
        ranked = self.rank()
        rows = [row for row in ranked["candidates"] if row["status"] == "ranked"]
        feasible = [row for row in rows if row["predicted_sec"] <= ranked["maximum_time_sec"]]
        anchors = (
            sorted(feasible, key=lambda row: (row["hourly_cost_units"], row["predicted_sec"]))[:8]
            if feasible
            else sorted(rows, key=lambda row: row["predicted_sec"])[:8]
        )
        if not anchors:
            return chosen + remaining[: count - len(chosen)]

        def distance(name: str) -> tuple[int, int]:
            point = self.features[name]
            nearest = min(
                sum(value != self.features[anchor["name"]][key] for key, value in point.items()) for anchor in anchors
            )
            return nearest, self.priority[name]

        remaining.sort(key=distance)
        return chosen + remaining[: count - len(chosen)]

    def run(self) -> dict:
        begin = time.monotonic()
        base = InferenceCapture(self.request).run(reuse=True)
        if base["status"] not in {"completed", "reused"}:
            return base
        models = select_models(
            self.request.deployment,
            Path(self.state["models"]) if "models" in self.state else self.request.models,
            self.request.database,
            self.output / "performance",
        )
        self.state["models"] = str(models)
        prepared = replace(
            self.request,
            base=BaseInput(Path(base["manifest"]), None),
            models=models,
            database=None,
            sampling_budget=None,
        )
        preparation_sec = time.monotonic() - begin
        resuming = bool(self.state["rounds"])
        round_index = 0
        new_predictions = 0
        while len(self.cells) < self.budgets[-1]:
            limit = next(value for value in self.budgets if value > len(self.cells))
            names = self.choose(min(BATCH_EVALUATIONS, limit - len(self.cells)))
            execution = replace(prepared.execution, output=self.output / "batches" / f"{round_index:03d}")
            report = execution.output / "prediction/report.json"
            if round_index < len(self.state["rounds"]):
                entry = self.state["rounds"][round_index]
                if entry["status"] == "running":
                    # A completed container may outlive its interrupted Python caller.
                    if not report.exists():
                        return {"status": "incomplete", "unfinished_batch": entry, "state": str(self.state_path)}
                    result = read_json(report)
                    if result["status"] not in {"completed", "incomplete"}:
                        return {"status": "incomplete", "unfinished_batch": entry, "state": str(self.state_path)}
                    entry.update(status="completed", wall_sec=None, timing_qualification="Recovered container result")
                result = read_json(Path(entry["report"]))
            else:
                entry = {"names": names, "report": str(report), "status": "running", "wall_sec": None}
                self.state["rounds"].append(entry)
                write_json(self.state_path, self.state)
                round_begin = time.monotonic()
                try:
                    result = predict(
                        replace(prepared, targets=tuple(self.targets[name] for name in names), execution=execution)
                    )
                except Exception as error:
                    entry.update(status="failed", error=str(error), wall_sec=time.monotonic() - round_begin)
                    write_json(self.state_path, self.state)
                    raise
                entry.update(status=result["status"], wall_sec=time.monotonic() - round_begin)
                new_predictions += len(names)

            if "cells" not in result:
                write_json(self.state_path, self.state)
                return {"status": "incomplete", "batch_result": result, "state": str(self.state_path)}
            self.cells.update({cell["name"]: cell for cell in result["cells"]})
            if not self.cells or self.rank()["status"] != "completed":
                write_json(self.state_path, self.state)
                return {"status": "incomplete", "batch_result": result, "state": str(self.state_path)}
            entry["status"] = "completed"
            entry.update(
                source_prepare_sec=result["source_prepare_sec"],
                simulation_sec=sum(cell["simulation_sec"] for cell in result["cells"]),
                maximum_process_rss_bytes=result["process_peak_rss_bytes"],
            )
            write_json(self.state_path, self.state)
            round_index += 1
            if len(self.cells) in self.budgets and not any(
                row["evaluations"] == len(self.cells) for row in self.state["checkpoints"]
            ):
                self.state["checkpoints"].append(
                    {
                        "evaluations": len(self.cells),
                        "wall_sec": None if resuming else time.monotonic() - begin,
                        "result": self.rank(),
                    }
                )
                write_json(self.state_path, self.state)

        result = self.rank()
        result.update(
            method=self.method,
            seed=self.seed,
            space_candidates=len(self.targets),
            evaluated=len(self.cells),
            new_predictions=new_predictions,
            preparation_sec=preparation_sec,
            run_wall_sec=time.monotonic() - begin,
            round_wall_sec=[entry["wall_sec"] for entry in self.state["rounds"]],
            checkpoints=self.state["checkpoints"],
            state=str(self.state_path),
            report=str(self.output / "report.json"),
        )
        result["qualification"] += (
            " Budgeted selection among requested candidates only; the full space is not evaluated. "
            "One maintained cost selection is shared by all batches; missing coverage stops this run without refitting. "
            "run_wall_sec is this invocation, including reuse/resume overhead; recovered batch time may be unknown."
        )
        write_json(self.output / "report.json", result)
        return result
