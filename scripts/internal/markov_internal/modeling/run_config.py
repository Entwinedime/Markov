"""Normalized contract for one container-side modeling run."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.paths import require_repo_path


@dataclass(frozen=True)
class ModelingOutputs:
    """Artifacts explicitly requested by a self-contained runner config."""

    dag_chrome_trace: bool = False
    module_summary: bool = False
    debug_logging: bool = False

    @classmethod
    def from_config(cls, config: dict[str, Any]) -> ModelingOutputs:
        """Derive output capabilities and implicit validation dependencies."""

        raw = config.get("outputs") if isinstance(config.get("outputs"), dict) else {}
        module_summary = bool(raw.get("emit_module_summary", False))
        return cls(
            dag_chrome_trace=bool(raw.get("emit_dag_chrome_trace", False)),
            module_summary=module_summary,
            debug_logging=bool(raw.get("debug", False)),
        )

    @property
    def requires_validation_backend(self) -> bool:
        """Return whether any requested artifact requires the Debug backend."""

        return self.module_summary


@dataclass(frozen=True)
class ModelingRunConfig:
    """Runner config after structural validation and path resolution."""

    path: Path
    raw: dict[str, Any]
    mode: str
    output_dir: Path
    profile_manifest: Path
    input_config: dict[str, Any]
    cpp_config: dict[str, Any]
    outputs: ModelingOutputs

    @classmethod
    def load(cls, path: Path) -> ModelingRunConfig:
        """Load one runner config and resolve every repository-relative path.

        Raises `TypeError` or `ValueError` before backend startup when the structural
        contract or requested backend capabilities are invalid.
        """

        raw = load_json(path)
        if not isinstance(raw, dict):
            raise TypeError(f"modeling config must be a JSON object: {path}")

        input_config = raw.get("input") if isinstance(raw.get("input"), dict) else {}
        manifest_value = input_config.get("profile_manifest")
        if not isinstance(manifest_value, str) or not manifest_value:
            raise ValueError("modeling config requires input.profile_manifest")

        output_value = raw.get("output_dir")
        if not isinstance(output_value, str) or not output_value:
            raise ValueError("modeling config requires output_dir")

        cpp_config = raw.get("cpp_trace_graph") if isinstance(raw.get("cpp_trace_graph"), dict) else {}
        outputs = ModelingOutputs.from_config(raw)
        if outputs.requires_validation_backend:
            require_validation_backend(cpp_config)

        return cls(
            path=path,
            raw=raw,
            mode=str(raw.get("mode") or "faithful_replay"),
            output_dir=require_repo_path(output_value),
            profile_manifest=require_repo_path(manifest_value),
            input_config=input_config,
            cpp_config=cpp_config,
            outputs=outputs,
        )


def require_validation_backend(cpp_config: dict[str, Any]) -> None:
    """Require the validation backend for diagnostics compiled behind DEBUG."""

    backend_kind = str(cpp_config.get("backend_kind") or "").strip().lower()
    if backend_kind == "validation":
        return
    raise ValueError("Debug modeling outputs require cpp_trace_graph.backend_kind='validation'")
