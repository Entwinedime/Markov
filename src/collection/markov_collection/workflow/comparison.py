"""Compare fixed predictions with independent, clock-associated serving ports."""

from __future__ import annotations

from collections import Counter, defaultdict
from os.path import relpath
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import read_json
from markov_collection.workflow.container_environment import ContainerEnvironment


class ServingComparison:
    def __init__(self, environment: ContainerEnvironment, project: Path, prediction: Path) -> None:
        self.environment = environment
        self.project_path = project
        self.project = read_json(project)
        self.prediction_path = prediction
        self.prediction = read_json(prediction)

    def _input(self, reference: str) -> Any:
        return read_json(self.environment.host_path(str(self.project_path.parent / reference)))

    def _context_matches(self, prepared: dict[str, Any]) -> bool:
        configuration = next(
            item for item in self.project["configurations"] if item["id"] == prepared["configuration_id"]
        )
        workload = next(
            item for item in self.project["request_workloads"] if item["id"] == prepared["request_workload_id"]
        )
        snapshot = self._input(configuration["config"])
        requests = [
            {
                "id": item["id"],
                "arrival_time_ns": item["arrival_offset_us"] * 1000,
                "input_tokens": item["input_tokens"],
                "output_tokens": item["output_tokens"],
            }
            for item in self._input(workload["path"])["requests"]
        ]
        return (
            prepared["framework"] == configuration["framework"]
            and prepared["model_path"] == self.project["model"]["path"]
            and prepared["tokenizer_path"] == self.project["model"]["tokenizer_path"]
            and prepared["environment"] == configuration["environment"]
            and prepared["configuration"] == {**snapshot["parameters"], **snapshot["model_assets"]}
            and prepared["requests"] == requests
        )

    def _observed(
        self, facts: dict[str, Any], requests: list[dict[str, Any]], ranks: list[int]
    ) -> tuple[dict[str, Any], dict[int, int]]:
        publications: dict[tuple[int, str], list[tuple[int, dict[str, Any]]]] = defaultdict(list)
        batches: Counter[int] = Counter()
        origin = facts["clock"]["origin_ns"]
        for port in facts["ports"]:
            if port["port"] == "submitted":
                batches[port["rank"]] += 1
            else:
                for request in port["result"]["requests"]:
                    publications[(port["rank"], request["id"])].append((port["at_ns"] - origin, request))

        metrics = []
        for request in requests:
            first = []
            completed = []
            for rank in ranks:
                progress = publications[(rank, request["id"])]
                first.append(min(at for at, item in progress if item["generated_tokens"] == 1))
                completed.append(
                    min(
                        at
                        for at, item in progress
                        if item["finished"] and item["generated_tokens"] == request["output_tokens"]
                    )
                )

            # Published means every rank has returned. CPU and scope-excluded
            # time remain in these native intervals; no residual is subtracted.
            first_ns, completed_ns = max(first), max(completed)
            arrival = request["arrival_time_ns"]
            if first_ns < arrival or completed_ns < first_ns:
                raise ValueError("native publication order does not match the request clock")
            metrics.append(
                {
                    "id": request["id"],
                    "first_published_ns": first_ns,
                    "completed_ns": completed_ns,
                    "ttft_ns": first_ns - arrival,
                    "request_latency_ns": completed_ns - arrival,
                    "tpot_ns": (completed_ns - first_ns) / (request["output_tokens"] - 1)
                    if request["output_tokens"] > 1
                    else None,
                }
            )
        window = max(item["completed_ns"] for item in metrics) - min(item["arrival_time_ns"] for item in requests)
        return {
            "window_time_ns": window,
            "output_tokens_per_sec": sum(item["output_tokens"] for item in requests) * 1_000_000_000 / window
            if window > 0
            else None,
            "requests": metrics,
        }, dict(sorted(batches.items()))

    def run(self, manifests: list[Path]) -> dict[str, Any]:
        combinations = {
            (item["configuration_id"], item["request_workload_id"]): item for item in self.prediction["combinations"]
        }
        observations = {}
        for path in manifests:
            manifest = read_json(path)
            if manifest["role"] != "validation" or manifest["observation_purpose"] != "ports":
                raise ValueError(f"comparison requires an independent ports manifest: {path}")
            prepared = read_json(self.environment.host_path(str(path.parent / manifest["input"])))
            key = (prepared["configuration_id"], prepared["request_workload_id"])
            if key not in combinations or key in observations:
                raise ValueError(f"unknown or repeated validation combination: {key}")
            observations[key] = (path, manifest, prepared)

        rows = []
        for key, predicted in combinations.items():
            row: dict[str, Any] = {
                "configuration_id": key[0],
                "request_workload_id": key[1],
                "status": "not_comparable",
                "scope": predicted["scope"],
                "timing_basis": predicted.get("timing_basis"),
            }
            rows.append(row)
            if predicted["status"] != "complete":
                row["reason"] = "prediction_incomplete"
                continue
            if key not in observations:
                row["reason"] = "validation_missing"
                continue

            path, manifest, prepared = observations[key]
            row["validation_manifest"] = (
                relpath(path, self.prediction_path.parent)
                if path.is_relative_to(self.prediction_path.parent.parent)
                else str(path)
            )
            row["gaps"] = manifest["gaps"]
            if not self._context_matches(prepared):
                row["reason"] = "validation_input_mismatch"
                continue
            if manifest["readiness"] != "observed" or manifest["gaps"]:
                row["reason"] = "validation_ports_or_clock_incomplete"
                continue

            facts = read_json(self.environment.host_path(str(path.parent / manifest["observations"])))
            try:
                measured, batches = self._observed(facts, prepared["requests"], manifest["coverage"]["expected_ranks"])
            except ValueError as error:
                row["reason"] = str(error)
                continue
            prediction = predicted["metrics"]
            observed_window = measured["window_time_ns"]
            bias = prediction["window_time_ns"] - observed_window
            row.update(
                status="compared",
                predicted_batch_count=predicted["execution_summary"]["batch_invocations"],
                observed_batch_count_by_rank=batches,
                window={
                    "predicted_ns": prediction["window_time_ns"],
                    "observed_ns": observed_window,
                    "bias_ns": bias,
                    "relative_error_percent": 100 * bias / observed_window if observed_window > 0 else None,
                },
                requests=[
                    {
                        "id": item["id"],
                        "predicted": next(metric for metric in prediction["requests"] if metric["id"] == item["id"]),
                        "observed": item,
                    }
                    for item in measured["requests"]
                ],
            )

        return {
            "prediction": self.prediction_path.name,
            "status": "complete" if all(item["status"] == "compared" for item in rows) else "incomplete",
            "purpose": "fixed prediction versus independent serving Published ports",
            "observed_completion": "latest return across all declared ranks",
            "comparison_scope": "native intervals include excluded host CPU and other out-of-scope time",
            "validation_used_for_templates_or_fitting": False,
            "accuracy_qualified": False,
            "combinations": rows,
        }
