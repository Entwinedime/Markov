"""Compare fixed predictions with independent serving ports or execution structure."""

from __future__ import annotations

import argparse
import fcntl
import json
import subprocess
from collections import Counter, defaultdict
from os.path import normpath, relpath
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import read_json, write_json
from markov_collection.workflow.container_environment import ContainerEnvironment
from markov_collection.workflow.modeling_job import ModelingJob


class StructureComparisonProcessing:
    def __init__(self, project: Path, prediction: Path, manifests: list[Path], directory: Path) -> None:
        self.project = project
        self.prediction = prediction
        self.manifests = manifests
        self.directory = directory

    def run(self) -> dict[str, Any]:
        # The named container owns the receipt, independently of its host client.
        with (self.directory / "processing.lock").open("a", encoding="utf-8") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                return read_json(self.directory / "result.json")
            except FileNotFoundError:
                pass

            binary = Path(__file__).resolve().parents[4] / "build/markov/markov"
            rows = []
            for manifest in self.manifests:
                process = subprocess.run(
                    [
                        str(binary),
                        "compare-structure",
                        "--project",
                        str(self.project),
                        "--prediction-result",
                        str(self.prediction),
                        "--validation-manifest",
                        str(manifest),
                    ],
                    check=True,
                    text=True,
                    stdout=subprocess.PIPE,
                )
                rows.append(json.loads(process.stdout))
            result = {"combinations": rows}
            write_json(self.directory / "result.json", result)
            return result


class ServingComparison:
    def __init__(self, environment: ContainerEnvironment, project: Path, prediction: Path) -> None:
        self.environment = environment
        self.project_path = project
        self.project = read_json(project)
        self.prediction_path = prediction
        self.prediction = read_json(prediction)
        self.purposes: set[str] = set()

    def observations(self, manifests: list[Path]) -> dict[str, list[tuple[Path, dict[str, Any]]]]:
        groups: dict[str, list[tuple[Path, dict[str, Any]]]] = {"ports": [], "structure": []}
        combinations = {
            (item["configuration_id"], item["request_workload_id"]) for item in self.prediction["combinations"]
        }
        seen = set()
        for path in manifests:
            manifest = read_json(path)
            purpose = manifest["observation_purpose"]
            if manifest["role"] != "validation" or purpose not in groups:
                raise ValueError(f"comparison requires independent ports or structure validation: {path}")
            prepared = read_json(self.environment.host_path(str(path.parent / manifest["input"])))
            combination = (prepared["configuration_id"], prepared["request_workload_id"])
            key = (*combination, purpose)
            if combination not in combinations or key in seen:
                raise ValueError(f"unknown or repeated validation combination: {key}")
            seen.add(key)
            groups[purpose].append((path, manifest))
        return groups

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
            and prepared["model_configuration"] == self.project["model"]["configuration"]
            and prepared["environment"] == configuration["environment"]
            and prepared["configuration"] == {**snapshot["parameters"], **snapshot["model_assets"]}
            and prepared["requests"] == requests
        )

    def _observed(
        self, facts: dict[str, Any], requests: list[dict[str, Any]], ranks: list[int]
    ) -> tuple[dict[str, Any], dict[int, int]]:
        publications: dict[tuple[int, str], list[tuple[int, dict[str, Any]]]] = defaultdict(list)
        arrivals: dict[tuple[int, str], list[int]] = defaultdict(list)
        batches: Counter[int] = Counter()
        origin = facts["clock"]["origin_ns"]
        for entry in facts["arrivals"]:
            arrivals[(entry["rank"], entry["request_id"])].append(entry["at_ns"] - origin)
        for port in facts["ports"]:
            if port["port"] == "submitted":
                batches[port["rank"]] += 1
        for output in facts["outputs"]:
            publications[(output["rank"], output["request_id"])].append((output["at_ns"] - origin, output))

        metrics = []
        for request in requests:
            first = []
            completed = []
            for rank in facts["publication_ranks"]:
                progress = publications[(rank, request["id"])]
                first.append(min(at for at, item in progress if item["tokens"] > 0))
                completed.append(
                    min(
                        at
                        for at, item in progress
                        if item["finished"] and item["cumulative_tokens"] == request["output_tokens"]
                    )
                )

            # Sending roles define token handoff at the modeled output boundary.
            # Private rank tails and all-rank entry spread remain diagnostics.
            first_ns, completed_ns = max(first), max(completed)
            arrival = max(arrivals[(rank, request["id"])][0] for rank in facts["publication_ranks"])
            rank_arrivals = [arrivals[(rank, request["id"])][0] for rank in ranks]
            if first_ns < arrival or completed_ns < first_ns:
                raise ValueError("native publication order does not match the request clock")
            metrics.append(
                {
                    "id": request["id"],
                    "native_arrival_ns": arrival,
                    "entry_skew_ns": arrival - request["arrival_time_ns"],
                    "rank_entry_spread_ns": max(rank_arrivals) - min(rank_arrivals),
                    "first_published_ns": first_ns,
                    "completed_ns": completed_ns,
                    "ttft_ns": first_ns - arrival,
                    "request_latency_ns": completed_ns - arrival,
                    "driver_ttft_ns": first_ns - request["arrival_time_ns"],
                    "driver_request_latency_ns": completed_ns - request["arrival_time_ns"],
                    "tpot_ns": (completed_ns - first_ns) / (request["output_tokens"] - 1)
                    if request["output_tokens"] > 1
                    else None,
                }
            )
        final = max(item["completed_ns"] for item in metrics)
        window = final - min(item["arrival_time_ns"] for item in requests)
        return {
            "window_time_ns": window,
            "output_tokens_per_sec": sum(item["output_tokens"] for item in requests) * 1_000_000_000 / window
            if window > 0
            else None,
            "requests": metrics,
        }, dict(sorted(batches.items()))

    def _ports(self, manifests: list[tuple[Path, dict[str, Any]]], previous: dict[str, Any]) -> dict[str, Any]:
        requests: dict[tuple[str, str], dict[str, Any]] = defaultdict(dict)
        with self.environment.host_path(self.prediction["request_results"]).open(encoding="utf-8") as stream:
            for line in stream:
                request = json.loads(line)
                requests[(request["configuration_id"], request["request_workload_id"])][request["id"]] = request
        combinations = {
            (item["configuration_id"], item["request_workload_id"]): item for item in self.prediction["combinations"]
        }
        observations = {}
        for path, manifest in manifests:
            prepared = read_json(self.environment.host_path(str(path.parent / manifest["input"])))
            key = (prepared["configuration_id"], prepared["request_workload_id"])
            observations[key] = (path, manifest, prepared)

        rows = []
        details = self.prediction_path.parent / "comparison-requests.jsonl"
        pending = details.with_suffix(".tmp")
        details_written = False
        retained = {
            (item["configuration_id"], item["request_workload_id"]): item
            for item in previous["combinations"]
            if item["purpose"] == "ports"
        }
        # Replacing one port observation preserves other combinations' results.
        # Structure comparison never rewrites these timing details.
        if previous["request_results"] is not None:
            with details.open(encoding="utf-8") as source, pending.open("w", encoding="utf-8") as stream:
                for line in source:
                    item = json.loads(line)
                    if (item["configuration_id"], item["request_workload_id"]) not in observations:
                        stream.write(line)
                        details_written = True

        for key, predicted in combinations.items():
            if key not in observations and key in retained:
                rows.append(retained[key])
                continue

            row: dict[str, Any] = {
                "configuration_id": key[0],
                "request_workload_id": key[1],
                "purpose": "ports",
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
            row["observation_boundaries"] = manifest["boundaries"]
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
                request_count=len(measured["requests"]),
            )

            with pending.open("a" if details_written else "w", encoding="utf-8") as stream:
                for item in measured["requests"]:
                    record = {
                        "configuration_id": key[0],
                        "request_workload_id": key[1],
                        "id": item["id"],
                        "predicted": requests[key][item["id"]],
                        "observed": item,
                    }
                    stream.write(json.dumps(record) + "\n")
            details_written = True

        if details_written:
            pending.replace(details)
        else:
            details.unlink(missing_ok=True)
            pending.unlink(missing_ok=True)

        return {
            "observed_completion": "latest token-bearing stream_output return of the observed sending roles",
            "comparison_scope": (
                "native scheduler readiness to request token handoff; all-rank internal progress retained separately"
            ),
            "combinations": rows,
            "request_results": details.name if details_written else None,
        }

    def _structures(self, manifests: list[tuple[Path, dict[str, Any]]], job: ModelingJob) -> list[dict[str, Any]]:
        binary = self.environment.root / "build/markov/markov"
        paths = {binary, self.environment.root / "src/collection", self.project_path, self.prediction_path}
        for item in self.project["configurations"]:
            paths.add(self.environment.host_path(str(self.project_path.parent / item["config"])))
        for item in self.project["request_workloads"]:
            paths.add(self.environment.host_path(str(self.project_path.parent / item["path"])))
        for path, manifest in manifests:
            paths.add(path)
            for field in ("input", "environment", "observations"):
                paths.add(self.environment.host_path(str(path.parent / manifest[field])))
        for combination in self.prediction["combinations"]:
            artifact = combination.get("structure", {}).get("graph", {}).get("artifact")
            if artifact is not None:
                graph = self.environment.host_path(str(self.prediction_path.parent / artifact))
                if graph.is_file():
                    paths.add(graph)

        # C++ owns context and structure validation. The host consumes only the
        # published receipt; Docker logs remain process diagnostics.
        arguments = [
            "python3",
            "-m",
            "markov_collection.workflow.comparison",
            "--project",
            self.environment.container_path(self.project_path),
            "--prediction-result",
            self.environment.container_path(self.prediction_path),
            "--directory",
            self.environment.container_path(job.directory),
        ]
        for path, _ in manifests:
            arguments += ["--validation-manifest", self.environment.container_path(path)]
        job.run(arguments, {Path(normpath(item)) for item in paths})
        return read_json(job.directory / "result.json")["combinations"]

    def run(self, manifests: list[Path], job: ModelingJob | None = None) -> dict[str, Any]:
        groups = self.observations(manifests)
        self.purposes = {purpose for purpose, items in groups.items() if items}

        try:
            previous = read_json(self.prediction_path.parent / "comparison.json")
        except FileNotFoundError:
            previous = {"combinations": [], "request_results": None}
        rows = {
            (item["configuration_id"], item["request_workload_id"], item["purpose"]): item
            for item in previous["combinations"]
        }
        updated = set()
        if groups["structure"] and job is None:
            raise ValueError("structure comparison requires a recorded modeling job")
        structures = self._structures(groups["structure"], job) if groups["structure"] else []
        for row in structures:
            key = (row["configuration_id"], row["request_workload_id"], "structure")
            if key in updated:
                raise ValueError(f"repeated structure validation combination: {key[:2]}")
            updated.add(key)
            rows[key] = row

        # Finish fallible structure consumers before publishing timing details.
        report = previous
        if groups["ports"]:
            report = self._ports(groups["ports"], previous)
            for row in report["combinations"]:
                rows[(row["configuration_id"], row["request_workload_id"], "ports")] = row

        report.update(
            prediction=self.prediction_path.name,
            status="complete" if all(row["status"] == "compared" for row in rows.values()) else "incomplete",
            purpose="fixed prediction versus independent target observations",
            validation_used_for_templates_or_fitting=False,
            accuracy_qualified=False,
            combinations=list(rows.values()),
            ports_complete=all(
                rows.get((item["configuration_id"], item["request_workload_id"], "ports"), {}).get("status")
                == "compared"
                for item in self.prediction["combinations"]
            ),
        )
        return report


def main() -> None:
    parser = argparse.ArgumentParser(description="Process one recorded structure comparison")
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--prediction-result", type=Path, required=True)
    parser.add_argument("--validation-manifest", type=Path, action="append", required=True)
    parser.add_argument("--directory", type=Path, required=True)
    arguments = parser.parse_args()
    StructureComparisonProcessing(
        arguments.project, arguments.prediction_result, arguments.validation_manifest, arguments.directory
    ).run()


if __name__ == "__main__":
    main()
