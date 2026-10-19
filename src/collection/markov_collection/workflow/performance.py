"""Produce shared cost candidates from explicit independent measurements."""

from __future__ import annotations

import argparse
import fcntl
import json
import subprocess
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import read_json, write_json
from markov_collection.workflow.container_environment import ContainerEnvironment
from markov_collection.workflow.modeling_job import ModelingJob
from markov_collection.workflow.run_directory import RunDirectory


class PerformanceProcessing:
    def __init__(self, manifest: Path, directory: Path, output_directory: Path) -> None:
        self.manifest = manifest
        self.directory = directory
        self.output_directory = output_directory

    def run(self) -> dict[str, Any]:
        # The container owns publication. A resumed host uses the same completed
        # receipt instead of fitting another copy or inferring success from files.
        with (self.directory / "processing.lock").open("a", encoding="utf-8") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                return read_json(self.directory / "result.json")
            except FileNotFoundError:
                pass

            root = Path(__file__).resolve().parents[4]
            process = subprocess.run(
                [
                    str(root / "build/markov/markov"),
                    "fit-costs",
                    "--performance-manifest",
                    str(self.manifest),
                    "--unit",
                    "all",
                ],
                check=True,
                stdout=subprocess.PIPE,
                text=True,
            )
            fitted = json.loads(process.stdout)
            models = []
            units = []
            for item in fitted["candidates"]:
                candidate = item["candidate"]
                if candidate["status"] == "candidate":
                    path = self.output_directory / f"{item['cost_unit']}.json"
                    write_json(path, candidate)
                    models.append(str(path))
                units.append(
                    {"unit": item["cost_unit"], "status": candidate["status"], "entries": len(candidate["entries"])}
                )

            result = {"manifest": fitted["source_manifest"], "cost_models": models, "units": units}
            write_json(self.directory / "result.json", result)
            return result


class PerformanceImport:
    def __init__(self, environment: ContainerEnvironment, manifest: Path, run: RunDirectory, ordinal: int) -> None:
        self.manifest = manifest
        self.output_directory = run.asset("costs", ordinal)
        task = f"fitting-{ordinal:03d}"
        self.job = ModelingJob(environment, run.container(task), run.work("fitting", ordinal), run.log(task))

    def run(self) -> dict[str, Any]:
        environment = self.job.environment
        directory = self.job.directory
        directory.mkdir(parents=True, exist_ok=True)
        # Mount only the declared normalized measurements and their context.
        # Original trace references remain provenance; target assets are absent.
        metadata = read_json(self.manifest)
        if metadata["role"] != "performance":
            raise ValueError("performance imports require independent performance measurements")
        paths = {
            self.manifest,
            environment.root / "build/markov/markov",
            environment.root / "src/collection",
        }
        for field in (
            "input",
            "environment",
            "native_capture",
            "device_measurements",
            "collective_measurements",
            "cpu_worker_units",
            "cpu_service_units",
        ):
            if field in metadata:
                paths.add(environment.host_path(str(self.manifest.parent / metadata[field])).resolve())
        for reference in metadata.get("worker_environments", []):
            paths.add(environment.host_path(str(self.manifest.parent / reference)).resolve())

        self.job.run(
            [
                "python3",
                "-m",
                "markov_collection.workflow.performance",
                "--manifest",
                environment.container_path(self.manifest),
                "--directory",
                environment.container_path(directory),
                "--output-directory",
                environment.container_path(self.output_directory),
            ],
            paths,
            {self.output_directory},
        )

        return read_json(directory / "result.json")


def main() -> None:
    parser = argparse.ArgumentParser(description="Fit declared independent measurement units")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    arguments = parser.parse_args()
    PerformanceProcessing(arguments.manifest, arguments.directory, arguments.output_directory).run()


if __name__ == "__main__":
    main()
