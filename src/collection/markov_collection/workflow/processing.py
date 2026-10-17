"""Normalize a native capture under a container-owned processing lock."""

from __future__ import annotations

import argparse
import fcntl
import json
import subprocess
from pathlib import Path
from typing import Any, Protocol

from markov_collection.artifacts.json_file import read_json, write_json


class CaptureNormalizer(Protocol):
    def extract(self) -> Path: ...
    def release(self) -> None: ...


class CaptureProcessing:
    def __init__(
        self,
        capture: Path,
        role: str,
        normalizer: CaptureNormalizer,
        directory: Path,
        template_directory: Path | None,
        work_directory: Path | None,
    ) -> None:
        self.capture = capture
        self.role = role
        self.normalizer = normalizer
        self.directory = directory
        self.template_directory = template_directory
        self.work_directory = work_directory
        self.root = Path(__file__).resolve().parents[4]

    def run(self) -> dict[str, Any]:
        self.directory.mkdir(parents=True, exist_ok=True)
        with (self.directory / "processing.lock").open("a", encoding="utf-8") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                with (self.directory / "processing_result.json").open(encoding="utf-8") as stream:
                    return json.load(stream)
            except FileNotFoundError:
                pass  # A stopped processor has not published a completed result.

            failure = self.directory / "processing_failure.json"
            failure.unlink(missing_ok=True)
            try:
                result = self._process()
                self.normalizer.release()
            except Exception as error:
                # Only the processor can declare its work terminal. Host command
                # failures and interrupts do not manufacture this record.
                write_json(failure, {"role": self.role, "error_type": type(error).__name__, "message": str(error)})
                raise

            write_json(self.directory / "processing_result.json", result)
            return result

    def _process(self) -> dict[str, Any]:
        normalized = self.directory / "normalized.json"
        if normalized.exists():
            manifest = Path(read_json(normalized)["manifest"])
        else:
            manifest = self.normalizer.extract()
            # Normalization is already published. A later interrupted compile
            # or export cleanup resumes from these facts, not the raw profiler.
            write_json(normalized, {"manifest": str(manifest)})

        if self.role == "base":
            # The C++ entry point owns template-adapter availability. Capture-only
            # frameworks still publish their normalized base manifest.
            information = read_json(manifest)
            compiled = information.get("compiled_templates")
            if compiled is not None and (manifest.parent / compiled).is_file():
                result = information
            else:
                arguments = [str(self.root / "build/markov/markov"), "extract", "--base-manifest", str(manifest)]
                if self.template_directory is not None:
                    arguments += ["--output-directory", str(self.template_directory)]
                if self.work_directory is not None:
                    arguments += ["--work-directory", str(self.work_directory)]
                extraction = subprocess.run(arguments, check=True, stdout=subprocess.PIPE, text=True)
                result = json.loads(extraction.stdout)
            # Detailed extraction facts already belong to the manifest. The
            # workflow receipt only confirms this producer's completed handoff.
            return {"base_manifest": str(manifest), "readiness": result["readiness"]}

        information = read_json(manifest)
        return {f"{self.role}_manifest": str(manifest), "readiness": information["readiness"]}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--role", choices=["base", "performance", "validation"], required=True)
    parser.add_argument("--directory", type=Path, help="processing receipts and lock")
    parser.add_argument("--template-directory", type=Path, help="published compiled templates")
    parser.add_argument("--work-directory", type=Path, help="intermediate source graph and instances")
    arguments = parser.parse_args()

    # This process entry point composes the installed adapters. The persisted
    # workflow above owns publication and recovery, not framework semantics.
    from markov_collection.adapters.frameworks.sglang.kernel_work import SGLangKernelWork
    from markov_collection.adapters.runtimes.ascend.normalization import (
        CollectiveCostNormalizer,
        DeviceCostNormalizer,
        ServingNormalizer,
    )
    from markov_collection.artifacts.serving_ports import ServingPortObservations

    capture = read_json(arguments.capture)
    if arguments.role == "validation" and capture["observation_purpose"] == "ports":
        normalizer = ServingPortObservations(arguments.capture)
    elif arguments.role != "performance":
        normalizer = ServingNormalizer(arguments.capture, arguments.role)
    else:
        prepared = read_json(arguments.capture.parent / capture["input"])
        if prepared["measurement_kind"] == "communication":
            normalizer = CollectiveCostNormalizer(arguments.capture)
        elif capture["framework"] == "sglang":
            normalizer = DeviceCostNormalizer(arguments.capture, SGLangKernelWork())
        else:
            raise ValueError(f"device cost adapter is not implemented for {capture['framework']}")

    result = CaptureProcessing(
        arguments.capture,
        arguments.role,
        normalizer,
        arguments.directory or arguments.capture.parent,
        arguments.template_directory,
        arguments.work_directory,
    ).run()
    print(json.dumps(result))


if __name__ == "__main__":
    main()
