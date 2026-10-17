"""Docker deployment and path mapping shared by native and modeling tasks."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Any


class ContainerEnvironment:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.compose = [
            "docker",
            "compose",
            "-p",
            "markov-next-redesign",
            "-f",
            str(root / "docker/compose/inference.yml"),
        ]

    def host_path(self, path: str) -> Path:
        declared = Path(path)
        if declared.is_relative_to("/workspace/trace-sim"):
            return self.root / declared.relative_to("/workspace/trace-sim")
        return declared

    def container_path(self, path: Path) -> str:
        if path.is_relative_to(self.root):
            return str(Path("/workspace/trace-sim") / path.relative_to(self.root))
        # Explicit external inputs keep their absolute names in both namespaces.
        return str(path)

    def service_image(self, service: str) -> str:
        definition = subprocess.run(
            self.compose + ["config", "--format", "json"], check=True, text=True, capture_output=True
        )
        return json.loads(definition.stdout)["services"][service]["image"]

    def native_entrypoint(self, service: str) -> list[str]:
        inspection = subprocess.run(
            ["docker", "image", "inspect", "--format", "{{json .Config.Entrypoint}}", self.service_image(service)],
            check=True,
            text=True,
            capture_output=True,
        )
        return json.loads(inspection.stdout)

    def container_state(self, container: str) -> dict[str, Any] | None:
        inspection = subprocess.run(
            ["docker", "inspect", "--format", "{{json .State}}", container],
            capture_output=True,
            text=True,
            check=False,
        )
        if inspection.returncode == 0:
            return json.loads(inspection.stdout)
        if "No such object" in inspection.stderr or "No such container" in inspection.stderr:
            return None
        raise subprocess.CalledProcessError(
            inspection.returncode, inspection.args, inspection.stdout, inspection.stderr
        )
