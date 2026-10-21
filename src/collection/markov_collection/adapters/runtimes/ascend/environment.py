"""Installed runtime provenance, read without importing or initializing a device backend."""

from __future__ import annotations

import importlib.metadata
import json
import os
import platform
import subprocess
from pathlib import Path
from typing import Any


class RuntimeEnvironment:
    @staticmethod
    def _cpu() -> dict[str, Any] | None:
        keys = {"model name", "CPU implementer", "CPU architecture", "CPU variant", "CPU part", "CPU revision"}
        identities = []
        for block in Path("/proc/cpuinfo").read_text(encoding="utf-8").split("\n\n"):
            fields = dict(line.split(":", 1) for line in block.splitlines() if ":" in line)
            identity = {name.strip(): value.strip() for name, value in fields.items() if name.strip() in keys}
            if identity and identity not in identities:
                identities.append(identity)
        return {"architecture": platform.machine(), "models": identities} if identities else None

    @staticmethod
    def _devices(devices: list[int]) -> list[dict[str, str]] | None:
        names = ("Chip Type", "Chip Name", "Chip Version", "Firmware Version")
        profiles = []
        for device in devices:
            try:
                result = subprocess.run(
                    ["npu-smi", "info", "-t", "board", "-i", str(device), "-c", "0"],
                    check=False,
                    text=True,
                    capture_output=True,
                )
            except FileNotFoundError:
                return None
            if result.returncode != 0:
                return None

            fields = dict(line.split(":", 1) for line in result.stdout.splitlines() if ":" in line)
            fields = {name.strip(): value.strip() for name, value in fields.items()}
            if any(not fields.get(name) for name in names):
                return None
            profile = {name: fields[name] for name in names}
            if profile not in profiles:
                profiles.append(profile)
        return profiles or None

    @staticmethod
    def workspace_probe() -> dict[str, Any] | None:
        try:
            package = importlib.metadata.distribution("torch-npu").locate_file("torch_npu")
        except importlib.metadata.PackageNotFoundError:
            return None
        try:
            return json.loads((package / "markov_workspace_build.json").read_text(encoding="utf-8"))
        except FileNotFoundError:
            return None

    def describe(self, devices: list[int] | None = None) -> dict[str, Any]:
        packages = {}
        for name in ("torch", "torch-npu"):
            try:
                packages[name] = importlib.metadata.version(name)
            except importlib.metadata.PackageNotFoundError:
                packages[name] = None

        # This is the declared installation, not proof of the libraries loaded
        # by every worker. Preserve file provenance and never infer a version.
        root = os.environ.get("ASCEND_HOME_PATH")
        cann = None
        if root is not None:
            components = {}
            for name in ("runtime", "hccl"):
                path = Path(root) / "share" / "info" / name / "version.info"
                try:
                    content = path.read_text(encoding="utf-8")
                except FileNotFoundError:
                    components[name] = {"source": str(path), "version": None, "timestamp": None}
                    continue

                fields = dict(line.split("=", 1) for line in content.splitlines() if "=" in line)
                components[name] = {
                    "source": str(path),
                    "version": fields.get("Version"),
                    "timestamp": fields.get("timestamp"),
                }

            cann = {"root": root, "components": components}

        atb_root = os.environ.get("ATB_HOME_PATH")
        atb = None
        if atb_root is not None:
            # ATB_HOME_PATH selects the ABI directory under <release>/atb.
            # Keep the installation metadata verbatim, including its commit.
            path = (Path(atb_root).parents[1] / "version.info").resolve()
            try:
                content = path.read_text(encoding="utf-8")
            except FileNotFoundError:
                content = None

            atb = {"root": atb_root, "source": str(path), "version_info": content}

        try:
            driver = Path("/usr/local/Ascend/driver/version.info").read_text(encoding="utf-8")
        except FileNotFoundError:
            driver = None

        # These are observed conditions, not a machine ID or a timing sample.
        # Installation paths and physical device indices stay in provenance.
        runtime = None
        complete_cann = cann is not None and all(
            component["version"] and component["timestamp"] for component in cann["components"].values()
        )
        if (
            driver is not None
            and complete_cann
            and all(packages.values())
            and (atb is None or atb["version_info"] is not None)
        ):
            runtime = {
                "packages": packages,
                "cann": {
                    name: {key: value[key] for key in ("version", "timestamp")}
                    for name, value in cann["components"].items()
                },
                "atb": atb["version_info"] if atb is not None else None,
                "driver": driver,
                "kernel": platform.release(),
            }
        measurement = {
            "cpu": self._cpu(),
            "device": self._devices(devices) if devices is not None else None,
            "runtime": runtime,
        }
        return {
            "packages": packages,
            "cann": cann,
            "atb": atb,
            "workspace_probe": self.workspace_probe(),
            "measurement_environment": measurement,
        }

    def observe(self, deployment: dict[str, Any]) -> dict[str, Any]:
        runtime = self.describe(deployment["devices"])
        declared = deployment.get("measurement")
        if declared is not None:
            observed = runtime["measurement_environment"]
            for part, condition in declared.items():
                if condition is not None and condition != observed[part]:
                    raise ValueError(f"native {part} environment differs from its declared measurement conditions")
        return runtime
