"""Shared inputs and lifetime of an explicitly requested native capture."""

import json
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Any


class CaptureSession(ABC):
    def __init__(self, prepared: dict[str, Any], directory: Path) -> None:
        self.prepared = prepared
        self.directory = directory
        self._worker_failed = False

    def _handle_worker_failure(self, signum: int, frame: Any) -> None:
        self._worker_failed = True
        raise RuntimeError("native scheduler worker failed; see the collection process log")

    def _model_configuration(self) -> dict[str, Any]:
        with (Path(self.prepared["model_path"]) / "config.json").open(encoding="utf-8") as stream:
            return json.load(stream)

    @abstractmethod
    def run(self) -> None:
        """Run the framework workload; the outer workflow publishes artifacts."""
