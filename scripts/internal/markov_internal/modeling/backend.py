"""Build and execute the narrow C++ TraceGraph command line."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from ..common.paths import ROOT_DIR
from ..common.process import run_command
from .run_config import ModelingRunConfig


def trace_graph_executable(backend_kind: str = "release") -> Path:
    """Select the backend; report its exact build command when unavailable."""

    backend_kind = backend_kind.strip().lower() or "release"
    if backend_kind not in {"release", "validation"}:
        raise ValueError(f"unknown cpp_trace_graph.backend_kind: {backend_kind}")

    directory = f"build/modeling/trace_graph-{backend_kind}"
    executable = ROOT_DIR / directory / "trace_graph"
    if executable.is_file():
        return executable

    build_type, debug = ("Debug", "ON") if backend_kind == "validation" else ("Release", "OFF")
    command = (
        "scripts/run.sh modeling -- bash -lc 'cmake -S src/modeling/trace_graph "
        f"-B {directory} -G Ninja -DCMAKE_BUILD_TYPE={build_type} -DTRACE_GRAPH_DEBUG={debug} "
        f"&& cmake --build {directory} --target trace_graph -j2'"
    )
    raise FileNotFoundError(f"missing {backend_kind} trace_graph executable at {directory}/trace_graph; run {command}")


def build_trace_graph_command(run: ModelingRunConfig) -> list[str]:
    """Translate a normalized runner config into the narrow C++ CLI."""

    command = [
        str(trace_graph_executable(run.backend_kind)),
        "--profile-manifest",
        str(run.profile_manifest),
        "--run-summary",
        str(run.output_dir / "run_summary.json"),
    ]
    for field, option in (
        ("threads", "--threads"),
        ("file_threads", "--file-threads"),
        ("trace_window_start_us", "--trace-window-start-us"),
        ("trace_window_end_us", "--trace-window-end-us"),
        ("actual_e2e_us", "--actual-e2e-us"),
    ):
        append_option(command, option, run.cpp_config.get(field))
    if run.trace_channels is not None:
        append_option(command, "--trace-channels", ",".join(run.trace_channels))
    if run.cpu_service_cost is not None:
        command.extend(["--cpu-service-cost", str(run.cpu_service_cost)])
    if run.outputs.debug_logging:
        command.append("--debug")
    if run.hicache_static_replay:
        command.append("--hicache-static-replay")
    if run.outputs.dag_chrome_trace:
        command.extend(["--graph-output", str(run.output_dir / "dag_chrome_trace.json")])
    if run.outputs.module_summary:
        command.extend(["--model-summary", str(run.output_dir / "model_summary.json")])
    if run.model_config_path is not None:
        command.extend(["--model-config", str(run.model_config_path)])
    return command


def append_option(command: list[str], name: str, value: Any) -> None:
    """Append a value-bearing CLI option when the value is configured."""

    if value is not None:
        command.extend([name, str(value)])


def execute_trace_graph(command: list[str]) -> None:
    """Execute TraceGraph and raise an actionable error on failure."""

    completed = run_command(command, capture_output=True)
    if completed.returncode == 0:
        return
    detail = completed.stderr.strip() or completed.stdout.strip() or "<no stdout/stderr>"
    raise RuntimeError(
        "C++ TraceGraph failed "
        f"(returncode={completed.returncode}, command={json.dumps(command, ensure_ascii=False)}): {detail}"
    )
