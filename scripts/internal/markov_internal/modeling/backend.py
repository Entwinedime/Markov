"""Build and execute the narrow C++ TraceGraph command line."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from ..common.process import run_command
from .cpp_config import trace_graph_executable
from .run_config import ModelingRunConfig
from .trace_channels import configured_trace_channels


def build_trace_graph_command(run: ModelingRunConfig, model_config_path: Path | None) -> list[str]:
    """Translate a normalized runner config into the narrow C++ CLI."""

    command = [
        str(trace_graph_executable(run.raw)),
        "--profile-manifest",
        str(run.profile_manifest),
        "--run-summary",
        str(run.output_dir / "run_summary.json"),
    ]
    append_trace_options(command, run.raw, run.cpp_config)
    if run.outputs.debug_logging:
        command.append("--debug")
    if run.outputs.dag_chrome_trace:
        command.extend(["--graph-output", str(run.output_dir / "dag_chrome_trace.json")])
    if run.outputs.module_summary:
        command.extend(["--model-summary", str(run.output_dir / "model_summary.json")])
    if model_config_path is not None:
        command.extend(["--model-config", str(model_config_path)])
    return command


def append_trace_options(command: list[str], config: dict[str, Any], cpp_config: dict[str, Any]) -> None:
    """Append trace concurrency, window, and channel selection."""

    append_option(command, "--threads", cpp_config.get("threads"))
    append_option(command, "--file-threads", cpp_config.get("file_threads"))
    append_option(command, "--trace-window-start-us", cpp_config.get("trace_window_start_us"))
    append_option(command, "--trace-window-end-us", cpp_config.get("trace_window_end_us"))
    append_option(command, "--actual-e2e-us", cpp_config.get("actual_e2e_us"))
    channels = configured_trace_channels(config)
    if channels is not None:
        append_option(command, "--trace-channels", ",".join(channels))


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
