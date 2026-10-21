"""Independent target observations after the run's prediction has been fixed."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import read_json, write_json
from markov_collection.workflow.collection import CaptureProcessingFailed, CollectionWorkflow
from markov_collection.workflow.comparison import ServingComparison


class ValidationWorkflow:
    def __init__(self, collection: CollectionWorkflow) -> None:
        self.collection = collection

    def run(self, directory: Path, state: dict[str, Any]) -> tuple[bool, bool]:
        validation = state["validation"]
        prediction = directory / state["prediction"]["result"]
        if validation["observations"] is None:
            validation["observations"] = [
                {
                    "configuration_id": item["configuration_id"],
                    "request_workload_id": item["request_workload_id"],
                    "status": "pending",
                    "manifest": None,
                }
                for item in read_json(prediction)["combinations"]
            ]
            write_json(directory / "run.json", state)

        for item in validation["observations"]:
            if item["status"] == "captured":
                continue

            # The parent workflow retries native acquisition on explicit resume.
            # A processing failure retains its capture;
            # only an explicit repair can replace its terminal failure receipt.
            if item["status"] == "pending":
                prepared = self.collection.prepare(
                    directory / "inputs/project.json",
                    "validation",
                    "transfer",
                    configuration_id=item["configuration_id"],
                    workload_id=item["request_workload_id"],
                    budget_sec=validation["budget_per_combination_sec"],
                    purpose="ports",
                )
                self.collection.select(
                    prepared, directory, state, max_wall_time_sec=validation["budget_per_combination_sec"]
                )
                item["status"] = "collecting"
                write_json(directory / "run.json", state)

            try:
                self.collection.collect(directory, state)
            except CaptureProcessingFailed:
                pass  # The recorded failure remains the authoritative outcome.
            capture = state["capture"]
            if capture["status"] != "captured":
                item.update(status=capture["status"], reason=capture.get("reason", "validation_incomplete"))
                write_json(directory / "run.json", state)
                break

            (manifest,) = capture["manifests"]
            item.update(status="captured", manifest=manifest)
            item.pop("reason", None)
            write_json(directory / "run.json", state)

        manifests = [Path(item["manifest"]) for item in validation["observations"] if item["manifest"] is not None]
        compared = False
        if manifests:
            comparison = ServingComparison(
                self.collection.environment, directory / "inputs/project.json", prediction
            ).run(manifests)
            relative = Path("results/comparison.json")
            write_json(directory / relative, comparison)
            state["comparison"] = str(relative)
            compared = comparison["ports_complete"]
        finished = all(item["status"] == "captured" for item in validation["observations"])
        return compared, finished
