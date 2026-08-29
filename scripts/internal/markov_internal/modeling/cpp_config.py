"""Narrow C++ model-config and executable selection helpers."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ..common.paths import ROOT_DIR, require_repo_path

TRACE_GRAPH_RELEASE_EXECUTABLE = ROOT_DIR / "build/modeling/trace_graph-release/trace_graph"
TRACE_GRAPH_VALIDATION_EXECUTABLE = ROOT_DIR / "build/modeling/trace_graph-validation/trace_graph"

TRACE_GRAPH_RELEASE_BUILD_COMMAND = (
    "scripts/run.sh modeling -- bash -lc 'cmake -S src/modeling/trace_graph "
    "-B build/modeling/trace_graph-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRACE_GRAPH_DEBUG=OFF "
    "&& cmake --build build/modeling/trace_graph-release --target trace_graph -j2'"
)
TRACE_GRAPH_VALIDATION_BUILD_COMMAND = (
    "scripts/run.sh modeling -- bash -lc 'cmake -S src/modeling/trace_graph "
    "-B build/modeling/trace_graph-validation -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRACE_GRAPH_DEBUG=ON "
    "&& cmake --build build/modeling/trace_graph-validation --target trace_graph -j2'"
)


def cpp_model_config_path(config: dict[str, Any], mode: str) -> Path | None:
    """Resolve the model config materialized by the workflow executor."""

    if mode == "faithful_replay":
        return None

    cpp_cfg = config.get("cpp_model_config")
    if not isinstance(cpp_cfg, str) or not cpp_cfg:
        raise ValueError(f"mode={mode} requires a cpp_model_config path")
    return require_repo_path(cpp_cfg)


def trace_graph_executable(config: dict[str, Any]) -> Path:
    """Select the Release or validation executable required by the run."""

    cpp = config.get("cpp_trace_graph") if isinstance(config.get("cpp_trace_graph"), dict) else {}
    backend_kind = str(cpp.get("backend_kind") or "").strip().lower()
    if backend_kind == "validation":
        if TRACE_GRAPH_VALIDATION_EXECUTABLE.is_file():
            return TRACE_GRAPH_VALIDATION_EXECUTABLE
        raise FileNotFoundError(
            "missing validation trace_graph executable at build/modeling/trace_graph-validation/trace_graph; "
            f"run {TRACE_GRAPH_VALIDATION_BUILD_COMMAND}"
        )
    if backend_kind not in {"", "release"}:
        raise ValueError(f"unknown cpp_trace_graph.backend_kind: {backend_kind}")
    if TRACE_GRAPH_RELEASE_EXECUTABLE.is_file():
        return TRACE_GRAPH_RELEASE_EXECUTABLE
    raise FileNotFoundError(
        "missing release trace_graph executable at build/modeling/trace_graph-release/trace_graph; "
        f"run {TRACE_GRAPH_RELEASE_BUILD_COMMAND}"
    )
