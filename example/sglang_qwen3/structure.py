#!/usr/bin/env python3
"""Collect representative structure, then expand with explicit uniform service durations."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path
from typing import Any

# Run directly from the checkout without installing a second host-side package.
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src/collection"))

from markov_collection.adapters.runtimes.ascend.capture import AscendCaptureRuntime  # noqa: E402
from markov_collection.artifacts.json_file import read_json, write_json  # noqa: E402
from markov_collection.workflow.collection import CollectionWorkflow  # noqa: E402
from markov_collection.workflow.prediction import PredictionWorkflow  # noqa: E402


class StructureExample:
    def __init__(self) -> None:
        collection = CollectionWorkflow(ROOT, AscendCaptureRuntime())
        self.workflow = PredictionWorkflow(collection)
        self.environment = collection.environment

    def start(self, model: Path, devices: list[int], service_ns: int) -> dict[str, Any]:
        subprocess.run([str(ROOT / "scripts/build.sh")], cwd=ROOT, check=True)
        result = self.workflow.predict(
            Path(__file__).with_name("structure-project.json"), model_path=model, devices=devices
        )
        return self._expand(result, service_ns)

    def resume(self, directory: Path, service_ns: int | None) -> dict[str, Any]:
        return self._expand(self.workflow.resume(directory), service_ns)

    def _expand(self, result: dict[str, Any], service_ns: int | None) -> dict[str, Any]:
        # Native lifecycle, retries, quotas and incomplete structure remain owned
        # by the public workflow. Only a finished handoff can receive givens.
        if (
            result["task_status"] != "finished"
            or result["status"] == "failed"
            or result["active_representative"] is not None
            or result.get("workflow_busy")
        ):
            return result

        directory = Path(result["run"])
        inputs = directory / "inputs"
        project = read_json(inputs / "project.json")
        sources = project["assets"]["templates"]
        if not sources or any(asset["status"] != "loaded" for asset in result["assets"]):
            return result

        declaration = inputs / "structure-template-assets.json"
        if declaration.exists():
            assets = read_json(declaration)
            recorded = read_json(inputs / assets[0]["node_durations"])["values"][0]["duration_ns"]
            if service_ns is not None and service_ns != recorded:
                raise ValueError("different givens require a new run or an explicit asset revision")
        else:
            if any(asset["node_durations"] is not None for asset in sources):
                raise ValueError("this run already declares other givens; use its public workflow directly")
            if service_ns is None:
                raise ValueError("initial givens are not published yet; resume with an explicit --service-ns")

            assets = []
            for ordinal, source in enumerate(sources, 1):
                filename = f"structure-given-{ordinal:03d}.json"
                write_json(
                    inputs / filename,
                    {
                        "base_manifest": source["manifest"],
                        "unit": "ns",
                        "assumptions": [
                            f"The user assigns every charged service {service_ns} ns, independent of its features.",
                            "Uniform givens exercise structure only; they are not measured or predicted performance.",
                            "CPU service, compute, communication, transfer and commands share the declared duration; "
                            "waits remain causal dependencies, not additional services.",
                        ],
                        "values": [
                            {"work": {"kind": kind}, "duration_ns": service_ns}
                            for kind in ("cpu", "kernel", "collective", "communication", "transfer", "device_command")
                        ],
                    },
                )
                assets.append(
                    {
                        "manifest": str(self.environment.host_path(source["manifest"])),
                        "node_durations": filename,
                    }
                )
            # Publish the complete declaration last; an interrupted attachment
            # resumes through the same public asset revision, never a new run.
            write_json(declaration, assets)

        expected = [
            {
                "manifest": self.environment.container_path(Path(asset["manifest"])),
                "node_durations": self.environment.container_path(inputs / asset["node_durations"]),
            }
            for asset in assets
        ]
        if sources == expected:
            return result
        if [source["manifest"] for source in sources] != [asset["manifest"] for asset in expected] or any(
            source["node_durations"] is not None for source in sources
        ):
            raise ValueError("template assets changed after given publication; use an explicit workflow revision")
        return self.workflow.resume(directory, template_assets=declaration)


def positive(value: str) -> int:
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("service duration must be a positive integer in ns")
    return number


def device_ids(value: str) -> list[int]:
    try:
        devices = [int(item) for item in value.split(",")]
    except ValueError:
        raise argparse.ArgumentTypeError("devices require comma-separated integer IDs") from None
    if any(item < 0 for item in devices) or len(set(devices)) != len(devices):
        raise argparse.ArgumentTypeError("device IDs must be nonnegative and unique")
    return devices


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--model", type=Path, help="absolute model/tokenizer path visible to the native container")
    mode.add_argument("--resume", type=Path, help="reuse the same run, inputs and cumulative base budget")
    parser.add_argument("--devices", type=device_ids, help="physical Ascend device IDs, e.g. 4")
    parser.add_argument("--service-ns", type=positive, help="explicit uniform duration for every charged service")
    parser.add_argument("--image-prefix", default=os.environ.get("MARKOV_IMAGE_PREFIX", "markov-next"))
    arguments = parser.parse_args()

    if arguments.resume is not None:
        if arguments.devices is not None:
            parser.error("--resume uses the recorded devices; omit --devices")
    elif arguments.devices is None or arguments.service_ns is None:
        parser.error("a new run requires --model, --devices and --service-ns")
    elif not arguments.model.is_absolute():
        parser.error("--model must be absolute and visible to the native container")

    os.environ["MARKOV_IMAGE_PREFIX"] = arguments.image_prefix
    example = StructureExample()
    if arguments.resume is not None:
        result = example.resume(arguments.resume.resolve(), arguments.service_ns)
    else:
        result = example.start(arguments.model, arguments.devices, arguments.service_ns)

    print(f"Run: {result['run']}\nStatus: {result['status']}")
    if result.get("reason"):
        print(f"Reason: {result['reason']}")
    for combination in result.get("combinations", []):
        print(f"  {combination['configuration_id']} / {combination['request_workload_id']}: {combination['status']}")
    if result["summary"] is not None:
        print(f"Summary: {Path(result['run']) / result['summary']}")
    raise SystemExit(0 if result["status"] == "complete" else 1 if result["status"] == "failed" else 2)


if __name__ == "__main__":
    main()
