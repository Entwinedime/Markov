"""Prepare a capture and compose the selected framework with Ascend exports."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.artifacts import AscendCaptureArtifacts
from markov_collection.artifacts.json_file import read_json
from markov_collection.instrumentation.session import CaptureSession


class NativeCapture:
    def __init__(self, prepared: dict[str, Any], directory: Path) -> None:
        self.prepared = prepared
        self.directory = directory

    def _session(self) -> CaptureSession:
        framework = self.prepared["framework"]
        if framework == "sglang":
            if self.prepared["role"] == "performance":
                from markov_collection.adapters.frameworks.sglang.sampling import SGLangSampling

                return SGLangSampling(self.prepared, self.directory)

            from markov_collection.adapters.frameworks.sglang.capture import SGLangCapture

            return SGLangCapture(self.prepared, self.directory)

        if framework == "ktransformers" and self.prepared["role"] != "performance":
            from markov_collection.adapters.frameworks.ktransformers.capture import KTransformersCapture

            return KTransformersCapture(self.prepared, self.directory)

        raise ValueError(f"unsupported {self.prepared['role']} capture framework: {framework}")

    def run(self) -> None:
        if not self.prepared["permission"]["allow_run"]:
            raise ValueError("native collection permission is disabled")

        session = self._session()
        # Capture log offsets before native startup. One outer owner publishes
        # vendor artifacts; direct target ports have no vendor exports or journal.
        ports_only = self.prepared["role"] == "validation" and self.prepared["observation_purpose"] == "ports"
        profiling = not ports_only and (
            self.prepared["role"] != "performance" or self.prepared["measurement_kind"] == "device"
        )
        artifacts = AscendCaptureArtifacts(self.prepared, self.directory) if profiling else None
        if self.prepared["role"] != "performance":
            self._prepare_requests()

        session.prepare()
        session.run()
        if artifacts is not None:
            artifacts.publish()

    def _prepare_requests(self) -> None:
        requests = self.prepared["requests"]
        if any("text" in request for request in requests):
            from transformers import AutoTokenizer

            tokenizer = AutoTokenizer.from_pretrained(self.prepared["tokenizer_path"], local_files_only=True)
            for request in requests:
                if "text" in request:
                    request["original_text"] = request.pop("text")
                    request["input_tokens"] = tokenizer.encode(request["original_text"])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    prepared = read_json(arguments.input)

    NativeCapture(prepared, arguments.output).run()


if __name__ == "__main__":
    main()
