"""Normalize a native capture under a container-owned processing lock."""

from __future__ import annotations

import argparse
import fcntl
import json
import subprocess
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.normalization import (
    CollectiveCostNormalizer,
    DeviceCostNormalizer,
    ServingNormalizer,
)
from markov_collection.artifacts.json_file import write_json


class CaptureProcessing:
    def __init__(self, capture: Path, role: str) -> None:
        self.capture = capture
        self.role = role
        self.directory = capture.parent
        self.root = Path(__file__).resolve().parents[4]

    def run(self) -> dict[str, Any]:
        with (self.directory / "processing.lock").open("a", encoding="utf-8") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                with (self.directory / "processing_result.json").open(encoding="utf-8") as stream:
                    return json.load(stream)
            except FileNotFoundError:
                pass  # A stopped processor has not published a completed result.

            if self.role in {"base", "validation"}:
                manifest = ServingNormalizer(self.capture, self.role).extract()
                with manifest.open(encoding="utf-8") as stream:
                    information = json.load(stream)
                if self.role == "validation":
                    result = {"validation_manifest": str(manifest), "readiness": information["readiness"]}
                elif information["framework"] == "sglang":
                    extraction = subprocess.run(
                        [str(self.root / "build/markov/markov"), "extract", "--base-manifest", str(manifest)],
                        check=True,
                        capture_output=True,
                        text=True,
                    )
                    result = json.loads(extraction.stdout)
                else:
                    result = {"base_manifest": str(manifest), "readiness": information["readiness"]}
            else:
                with (self.directory / "prepared_input.json").open(encoding="utf-8") as stream:
                    prepared = json.load(stream)

                normalizer = (
                    CollectiveCostNormalizer(self.capture)
                    if prepared["measurement_kind"] == "communication"
                    else DeviceCostNormalizer(self.capture)
                )
                manifest = normalizer.extract()
                with manifest.open(encoding="utf-8") as stream:
                    information = json.load(stream)
                result = {"performance_manifest": str(manifest), "readiness": information["readiness"]}

            # Publish the resumable result only after normalization and extraction finish.
            write_json(self.directory / "processing_result.json", result)
            return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--role", choices=["base", "performance", "validation"], required=True)
    arguments = parser.parse_args()

    print(json.dumps(CaptureProcessing(arguments.capture, arguments.role).run()))


if __name__ == "__main__":
    main()
