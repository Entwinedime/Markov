"""Normalized contract for one container-side modeling run."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.paths import repo_relative_path, require_repo_path


@dataclass(frozen=True)
class ModelingOutputs:
    """Artifacts explicitly requested by a self-contained runner config."""

    dag_chrome_trace: bool = False
    module_summary: bool = False
    debug_logging: bool = False


@dataclass(frozen=True)
class ModelingRunConfig:
    """Runner config after structural validation and path resolution."""

    output_dir: Path
    profile_manifest: Path
    cpp_config: dict[str, Any]
    outputs: ModelingOutputs
    backend_kind: str = "release"
    model_config_path: Path | None = None
    cpu_service_cost: Path | None = None
    trace_channels: tuple[str, ...] | None = None
    hicache_static_replay: bool = False

    def to_raw(self) -> dict[str, Any]:
        """Serialize the execution inputs for replay through the public config entry."""

        inputs = {"profile_manifest": str(repo_relative_path(self.profile_manifest))}
        if self.cpu_service_cost is not None:
            inputs["cpu_service_cost"] = str(repo_relative_path(self.cpu_service_cost))
        cpp = {**self.cpp_config, "backend_kind": self.backend_kind}
        if self.trace_channels is not None:
            cpp["trace_channels"] = list(self.trace_channels)
        if self.hicache_static_replay:
            cpp["hicache_static_replay"] = True
        raw: dict[str, Any] = {
            "input": inputs,
            "output_dir": str(repo_relative_path(self.output_dir)),
            "cpp_trace_graph": cpp,
            "outputs": {
                "emit_dag_chrome_trace": self.outputs.dag_chrome_trace,
                "emit_module_summary": self.outputs.module_summary,
                "debug": self.outputs.debug_logging,
            },
        }
        if self.model_config_path is not None:
            raw["cpp_model_config"] = str(repo_relative_path(self.model_config_path))
        return raw

    @classmethod
    def load(cls, path: Path) -> ModelingRunConfig:
        """Load one runner config and resolve every repository-relative path.

        Raises `TypeError` or `ValueError` before backend startup when the structural
        contract or requested backend capabilities are invalid.
        """

        return cls.from_raw(load_json(path))

    @classmethod
    def from_raw(cls, raw: dict[str, Any]) -> ModelingRunConfig:
        """Normalize file or in-memory input through the same runner contract."""

        if not isinstance(raw, dict):
            raise TypeError("modeling config must be a JSON object")

        input_config = raw.get("input") if isinstance(raw.get("input"), dict) else {}
        manifest_value = input_config.get("profile_manifest")
        if not isinstance(manifest_value, str) or not manifest_value:
            raise ValueError("modeling config requires input.profile_manifest")

        output_value = raw.get("output_dir")
        if not isinstance(output_value, str) or not output_value:
            raise ValueError("modeling config requires output_dir")

        cpp_config = raw.get("cpp_trace_graph") if isinstance(raw.get("cpp_trace_graph"), dict) else {}
        output_flags = raw.get("outputs") if isinstance(raw.get("outputs"), dict) else {}
        outputs = ModelingOutputs(
            dag_chrome_trace=bool(output_flags.get("emit_dag_chrome_trace", False)),
            module_summary=bool(output_flags.get("emit_module_summary", False)),
            debug_logging=bool(output_flags.get("debug", False)),
        )
        backend_kind = str(cpp_config.get("backend_kind") or "release").strip().lower()
        static_replay = bool(cpp_config.get("hicache_static_replay", False))
        if (outputs.module_summary or static_replay) and backend_kind != "validation":
            raise ValueError("Debug modeling outputs require cpp_trace_graph.backend_kind='validation'")
        service_cost = input_config.get("cpu_service_cost")
        model_config = raw.get("cpp_model_config")
        if model_config is not None and (not isinstance(model_config, str) or not model_config):
            raise ValueError("cpp_model_config must be a model configuration path")

        return cls(
            output_dir=require_repo_path(output_value),
            profile_manifest=require_repo_path(manifest_value),
            cpp_config=cpp_config,
            outputs=outputs,
            backend_kind=backend_kind,
            model_config_path=require_repo_path(model_config) if model_config is not None else None,
            cpu_service_cost=require_repo_path(service_cost) if service_cost else None,
            trace_channels=_trace_channels(cpp_config.get("trace_channels")),
            hicache_static_replay=static_replay,
        )


def _trace_channels(raw: Any) -> tuple[str, ...] | None:
    """Normalize an external channel list; None or 'all' selects all channels."""

    if raw is None:
        return None
    if isinstance(raw, str):
        raw = raw.split(",")
    if not isinstance(raw, list):
        raise TypeError("trace_channels must be a comma-separated string or a list")

    channels = []
    for value in raw:
        token = str(value).strip().lower()
        if not token:
            continue
        if token == "all":
            return None
        if token not in {"torch", "ld_preload", "python_probe"}:
            raise ValueError(f"unknown trace channel: {token}")
        if token not in channels:
            channels.append(token)
    return tuple(channels) or None
