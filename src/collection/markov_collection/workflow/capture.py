"""Prepare a capture and compose the selected framework with Ascend exports."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

from markov_collection.adapters.runtimes.ascend.artifacts import AscendCaptureArtifacts
from markov_collection.artifacts.json_file import write_json
from markov_collection.instrumentation.session import CaptureSession


class NativeCapture(CaptureSession):
    def run(self) -> None:
        if not self.prepared["permission"]["allow_run"]:
            raise ValueError("native collection permission is disabled")

        artifacts = AscendCaptureArtifacts(self.prepared, self.directory)
        requests = self.prepared["requests"]
        if any("text" in request for request in requests):
            from transformers import AutoTokenizer

            tokenizer = AutoTokenizer.from_pretrained(self.prepared["tokenizer_path"], local_files_only=True)
            for request in requests:
                if "text" in request:
                    request["original_text"] = request.pop("text")
                    request["input_tokens"] = tokenizer.encode(request["original_text"])

        write_json(self.directory / "prepared_input.json", self.prepared)
        os.environ["MARKOV_PREPARED_INPUT"] = str(self.directory / "prepared_input.json")
        os.environ["MARKOV_OBSERVATIONS"] = str(self.directory / "observations")

        if self.prepared["framework"] == "ktransformers":
            from markov_collection.adapters.frameworks.ktransformers.capture import KTransformersCapture

            KTransformersCapture(self.prepared, self.directory).run()
        elif self.prepared["framework"] == "sglang":
            from markov_collection.adapters.frameworks.sglang.capture import SGLangCapture

            SGLangCapture(self.prepared, self.directory).run()
        else:
            raise ValueError(f"unsupported capture framework: {self.prepared['framework']}")

        artifacts.publish()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    with arguments.input.open(encoding="utf-8") as stream:
        prepared = json.load(stream)

    NativeCapture(prepared, arguments.output).run()


if __name__ == "__main__":
    main()
