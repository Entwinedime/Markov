"""Installed runtime provenance, read without importing or initializing a device backend."""

from __future__ import annotations

import importlib.metadata
import json
import os
from pathlib import Path
from typing import Any


class RuntimeEnvironment:
    def describe(self) -> dict[str, Any]:
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

        workspace_probe = None
        if packages["torch-npu"] is not None:
            package = importlib.metadata.distribution("torch-npu").locate_file("torch_npu")
            try:
                workspace_probe = json.loads((package / "markov_workspace_build.json").read_text(encoding="utf-8"))
            except FileNotFoundError:
                # An uninstrumented installation remains explicit in the evidence.
                # Package version alone does not establish observation coverage.
                pass

        return {"packages": packages, "cann": cann, "atb": atb, "workspace_probe": workspace_probe}
