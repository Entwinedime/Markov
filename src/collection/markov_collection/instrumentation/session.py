"""Shared inputs and lifetime of an explicitly requested native capture."""

import os
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import read_json, write_json


class CaptureSession(ABC):
    def __init__(self, prepared: dict[str, Any], directory: Path) -> None:
        self.prepared = prepared
        self.directory = directory
        self._worker_failed = False

    def prepare(self) -> None:
        actual = read_json(Path(self.prepared["model_path"]) / "config.json")
        if actual != self.prepared["model_configuration"]:
            raise ValueError("native model configuration differs from the capture input snapshot")

        write_json(self.directory / "prepared_input.json", self.prepared)
        os.environ["MARKOV_PREPARED_INPUT"] = str(self.directory / "prepared_input.json")
        os.environ["MARKOV_OBSERVATIONS"] = str(self.directory / "observations")

    def _handle_worker_failure(self, signum: int, frame: Any) -> None:
        self._worker_failed = True
        raise RuntimeError("native scheduler worker failed; see the collection process log")

    @abstractmethod
    def run(self) -> None:
        """Run the framework workload; the outer workflow publishes artifacts."""
