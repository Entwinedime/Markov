"""Docker deployment and path mapping shared by native and modeling tasks."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import read_json


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

    def image_id(self, service: str) -> str:
        inspection = subprocess.run(
            ["docker", "image", "inspect", "--format", "{{.Id}}", self.service_image(service)],
            check=True,
            text=True,
            capture_output=True,
        )
        return inspection.stdout.strip()

    def snapshot_project(self, inputs: dict[str, Any]) -> None:
        model = inputs["model"]
        if model["configuration"] is None:
            model["configuration"] = read_json(self.host_path(model["path"]) / "config.json")

        # The reader resolves declared paths; persist asset references in the
        # same execution namespace used by their later container mounts.
        assets = inputs["assets"]
        for template in assets["templates"]:
            for field in ("manifest", "node_durations"):
                if template[field] is not None:
                    template[field] = self.container_path(Path(template[field]))
        for field in ("performance_manifests", "cost_models"):
            assets[field] = [self.container_path(Path(path)) for path in assets[field]]

        # Resolve each service once. Offline inputs may already declare a saved
        # identity; prediction does not require that native image or any device.
        images = {}
        for item in inputs["configurations"]:
            environment = item["environment"]
            if environment["image_id"]:
                continue
            service = environment["service"]
            if service not in images:
                images[service] = self.image_id(service)
            environment["image_id"] = images[service]

    def snapshot_capture(self, prepared: dict[str, Any]) -> None:
        if prepared["model_configuration"] is None:
            prepared["model_configuration"] = read_json(self.host_path(prepared["model_path"]) / "config.json")
        environment = prepared["environment"]
        if not environment["image_id"]:
            environment["image_id"] = self.image_id(environment["service"])

    def native_entrypoint(self, image_id: str) -> list[str]:
        inspection = subprocess.run(
            ["docker", "image", "inspect", "--format", "{{json .Config.Entrypoint}}", image_id],
            check=True,
            text=True,
            capture_output=True,
        )
        # No ENTRYPOINT means Docker executes the declared command directly.
        return json.loads(inspection.stdout) or []

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

    def consume_container(self, container: str) -> None:
        """Release an acknowledged owner after its actual terminal state."""
        state = self.container_state(container)
        if state is None:
            return
        if state["Running"] or state["Status"] == "created":
            raise RuntimeError(f"cannot consume an unfinished container: {container}")
        subprocess.run(["docker", "rm", container], check=True, stdout=subprocess.DEVNULL)
