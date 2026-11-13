"""Resolve one group's missing costs with approved facts or bounded shared captures."""

from __future__ import annotations

from dataclasses import asdict, replace
from pathlib import Path
import time

from markov.artifacts.performance import PerformanceData
from markov.artifacts.request import Deployment, PredictionRequest, read_json
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

    def prepare(self, report: dict, models: dict[int, Path]) -> dict[int, Path] | None:
        updated = dict(models)
        for tp, path in models.items():
            cells = [cell for cell in report["cells"] if cell["deployment"]["tensor_parallel"] == tp]
            requirements = [item for item in report["cost_requirements"] if item["deployment"]["tensor_parallel"] == tp]
            if not requirements:
                continue
            deployment = Deployment.parse(cells[0]["deployment"])
            if len(self.task.settings.devices) < tp:
                raise ValueError(f"TP={tp} sampling needs at least {tp} declared capture devices")
            group = {"cells": cells, "cost_requirements": requirements, "sampling_context": cells[0]["sampling_context"]}
            selected = self.prepare_models(deployment, group, path)
            if selected is None:
                return None
            updated[tp] = selected
        return updated

    def prepare_models(self, deployment: Deployment, report: dict, models: Path) -> Path | None:
        if self.request.deployment.framework == "sglang":
            from markov.adapters.sglang.sampling import sampling_commands
        else:
            return None

        commands = sampling_commands(deployment, report, report["cost_requirements"])
        if not commands:
            return None
        summary = {"requirements": report["cost_requirements"], "captures": []}
        self.rounds.append(summary)
        for command in commands:
            begin = time.monotonic()
            try:
                selected = read_json(models)
                training = self.approved_training(selected, command.measurement_adapter, deployment)
                existing = self.task.completed_capture(command, training)
            finally:
                self.task.record_processing(time.monotonic() - begin)

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
                keys = ("operation", "implementation", "timing_scope")
                responses = {tuple(entry["work"][key] for key in keys) for entry in report["cost_requirements"]}
                adapter = ADAPTERS[command.measurement_adapter]
                available = adapter.recipes(adapter.metadata(artifact))

                # Fill other absent kinds from this capture without replacing
                # ready responses or requiring another prediction pass for each.
                responses.update(set(available) - {tuple(model[key] for key in keys) for model in selected["models"]})
                raw = {
                    "database": str(self.request.database),
                    "deployment": asdict(deployment),
                    "imports": [{"adapter": command.measurement_adapter, "capture": str(artifact)}],
                    "training": [str(path) for path in sorted(training | {artifact})],
                    "responses": [dict(zip(keys, kind)) for kind in sorted(responses)],
                    "models": str(models),
                    "output": str(self.directory / f"tp{deployment.tensor_parallel}" / "selected_models.json"),
                }
                path = self.directory / f"tp{deployment.tensor_parallel}" / "maintenance.json"
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

    def approved_training(self, selected: dict, adapter: str, deployment: Deployment) -> set[Path]:
        # Keep prior anchors for required kinds. Other responses stay selected,
        # even when these same captures contain a different measurement scope.
        sources = {Path(path) for model in selected["models"] for path in model["sources"]}
        store = PerformanceData(self.request.database, readonly=True)
        try:
            selected = set()
            for source in sources:
                capture = store.capture_metadata(source)
                if capture is None or capture.adapter != adapter:
                    continue
                if capture.context == ADAPTERS[adapter].context(capture.metadata, asdict(deployment)):
                    selected.add(Path(capture.identity))
            return selected
        finally:
            store.close()
