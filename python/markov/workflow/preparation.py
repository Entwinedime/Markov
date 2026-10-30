"""Resolve one group's missing costs with approved facts or bounded shared captures."""

from __future__ import annotations

from dataclasses import asdict, replace
from pathlib import Path
import time

from markov.artifacts.capture import DeviceCommand
from markov.artifacts.performance import PerformanceData, encoded
from markov.artifacts.request import PredictionRequest, read_json
from markov.artifacts.writing import write_json
from markov.workflow.device_task import CaptureBudgetExceeded, DeviceTask
from markov.workflow.performance import ADAPTERS, maintain_performance


class PerformancePreparation:
    def __init__(self, request: PredictionRequest):
        if request.execution.capture is None:
            raise ValueError("Shared performance sampling needs execution.capture to declare its image and devices")
        self.request = request
        self.directory = request.execution.output / "performance"
        settings = replace(request.execution.capture, budget=request.sampling_budget)
        self.task = DeviceTask(settings, self.directory / "sampling")
        self.task.recover()
        self.rounds = []

    def prepare(self, report: dict, models: Path) -> Path | None:
        if self.request.deployment.framework == "sglang":
            from markov.adapters.sglang.sampling import sampling_commands
        elif self.request.deployment.framework == "ktransformers":
            from markov.adapters.ktransformers.sampling import sampling_commands
        else:
            return None

        commands = sampling_commands(self.request.deployment, report, report["cost_requirements"])
        if not commands:
            return None
        summary = {"requirements": report["cost_requirements"], "captures": []}
        self.rounds.append(summary)
        for command in commands:
            begin = time.monotonic()
            existing = self.find_capture(command)
            if existing is None:
                try:
                    result = self.task.run(command)
                except CaptureBudgetExceeded as error:
                    summary["failure"] = str(error)
                    return None
                if result["status"] != "completed":
                    summary["failure"] = result
                    return None
                artifact = Path(result[command.result_key])
                reused = False
            else:
                artifact = existing
                reused = True
            capture_sec = time.monotonic() - begin
            begin = time.monotonic()
            try:
                training = self.approved_training(models, command.measurement_adapter, report["cost_requirements"])
                raw = {
                    "database": str(self.request.database),
                    "deployment": asdict(self.request.deployment),
                    "imports": [{"adapter": command.measurement_adapter, "capture": str(artifact)}],
                    "training": [str(path) for path in sorted(training | {artifact})],
                    "models": str(models),
                    "output": str(self.directory / "selected_models.json"),
                }
                path = self.directory / "maintenance.json"
                write_json(path, raw)
                result = maintain_performance(path)
                models = Path(result["models"])
            finally:
                self.task.record_processing(time.monotonic() - begin)
            summary["captures"].append(
                {
                    "adapter": command.measurement_adapter,
                    "capture": str(artifact),
                    "reused": reused,
                    "capture_sec": capture_sec,
                    "trained_models": result["trained_models"],
                }
            )
            if self.task.remaining()["wall_sec"] < 0:
                summary["failure"] = "The cumulative performance budget was reached during maintenance"
                return None
        return models

    def find_capture(self, command: DeviceCommand) -> Path | None:
        wanted = encoded(command.payload)
        for attempt in self.task.state["attempts"]:
            if attempt["status"] != "completed" or attempt["result_key"] != command.result_key:
                continue
            output = Path(attempt["output"])
            actual = read_json(output / "capture_input.json")
            actual.pop("budget", None)
            if encoded(actual) == wanted:
                return output / attempt["artifact"]
        return None

    def approved_training(self, models: Path, adapter: str, requirements: list[dict]) -> set[Path]:
        wanted = {
            tuple(item["work"][key] for key in ("operation", "implementation", "timing_scope")): set(
                item["work"]["features"]
            )
            for item in requirements
            if "work" in item
        }
        sources = set()
        for model in read_json(models)["models"]:
            kind = tuple(model[key] for key in ("operation", "implementation", "timing_scope"))
            if kind in wanted and set(model["ranges"]) <= wanted[kind]:
                sources.update(Path(path) for path in model["sources"])
        store = PerformanceData(self.request.database)
        try:
            selected = set()
            for source in sources:
                capture = store.capture_metadata(source)
                if capture is None or capture["adapter"] != adapter:
                    continue
                if capture["context"] == ADAPTERS[adapter].context(
                    capture["metadata"], asdict(self.request.deployment)
                ):
                    selected.add(source)
            return selected
        finally:
            store.close()
