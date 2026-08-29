"""Environment construction for profiled server and workload processes."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

from ..common.paths import ROOT_DIR, resolve_repo_path
from .drain import python_probe_flush_interval_seconds
from .frameworks import FrameworkAdapter
from .probe_targets import select_python_probe_targets
from .runtime import (
    RunLayout,
    channel_config,
    expand_layout_placeholders,
    parse_model_path,
    resolve_run_path,
)


PYTHON_PROBE_ROOT = ROOT_DIR / "src/profiling/python_probe"
BENCH_ENV_REMOVE_KEYS = (
    "LD_PRELOAD",
    "HOOK_TRACE_OUTPUT",
    "TRACE_SIM_PYTHON_PROBE",
    "TRACE_SIM_PYTHON_PROBE_TARGETS",
    "TRACE_SIM_PYTHON_PROBE_OUTPUT",
    "TRACE_SIM_PYTHON_PROBE_DEBUG",
    "TRACE_SIM_PYTHON_PROBE_FLUSH_EVERY",
    "TRACE_SIM_PYTHON_PROBE_FLUSH_INTERVAL_SEC",
)


def build_server_env(
    cfg: dict[str, Any], runtime: Any, layout: RunLayout, adapter: FrameworkAdapter
) -> dict[str, str]:
    """Build the server environment with only enabled capture channels."""

    env = os.environ.copy()
    for key, value in cfg.get("env", {}).items():
        env[str(key)] = expand_layout_placeholders(str(value), layout, cfg)

    apply_framework_defaults(env, adapter)
    if adapter.profiler_api:
        env["SGLANG_TORCH_PROFILER_DIR"] = str(layout.torch_trace_dir)
    env["TRACE_SIM_PROFILING_CHANNELS"] = ",".join(runtime.channels)

    if runtime.enabled and "python_probe" in runtime.channels:
        apply_python_probe_env(env, cfg, runtime, layout)
    if runtime.enabled and "ld_preload" in runtime.channels:
        apply_ld_preload_env(env, cfg, layout, adapter)
    return env


def apply_framework_defaults(env: dict[str, str], adapter: FrameworkAdapter) -> None:
    """Apply shared Ascend defaults and the selected framework's defaults."""

    env.setdefault("HOOK_ASCENDCL_SO_PATH", "/usr/local/Ascend/ascend-toolkit/latest/lib64/libascendcl.so")
    env.setdefault("PYTORCH_NPU_ALLOC_CONF", "expandable_segments:True")
    env.setdefault("STREAMS_PER_DEVICE", "32")
    env.setdefault("HCCL_BUFFSIZE", "1536")
    env.setdefault("HCCL_OP_EXPANSION_MODE", "AIV")
    if adapter.name == "sglang":
        env.setdefault("SGLANG_SET_CPU_AFFINITY", "1")


def apply_python_probe_env(env: dict[str, str], cfg: dict[str, Any], runtime: Any, layout: RunLayout) -> None:
    """Inject Python probe activation, target contracts, and output paths."""

    python_probe = channel_config(cfg, "python_probe")
    selected_targets = select_python_probe_targets(
        tuple(runtime.python_consumers),
        diagnostics=runtime.python_diagnostics,
    )
    prepend_pythonpath(env, PYTHON_PROBE_ROOT)
    env["TRACE_SIM_PYTHON_PROBE"] = "1"
    env["TRACE_SIM_PYTHON_PROBE_TARGETS"] = json.dumps(selected_targets, ensure_ascii=False)
    env["TRACE_SIM_PYTHON_PROBE_OUTPUT"] = str(layout.trace_dir / "python_probe")
    flush_every = python_probe.get("flush_every")
    if flush_every is not None:
        env["TRACE_SIM_PYTHON_PROBE_FLUSH_EVERY"] = str(flush_every)
    flush_interval_sec = python_probe_flush_interval_seconds(cfg)
    if flush_interval_sec > 0:
        env["TRACE_SIM_PYTHON_PROBE_FLUSH_INTERVAL_SEC"] = f"{flush_interval_sec:g}"
    if runtime.debug:
        env["TRACE_SIM_PYTHON_PROBE_DEBUG"] = "1"


def apply_ld_preload_env(
    env: dict[str, str], cfg: dict[str, Any], layout: RunLayout, adapter: FrameworkAdapter
) -> None:
    """Inject the LD_PRELOAD hook and bind its output to the current run."""

    ld_preload = channel_config(cfg, "ld_preload")
    if not ld_preload.get("enabled", True):
        return
    hook_lib = resolve_repo_path(ld_preload.get("library", adapter.hook_library))
    if hook_lib is None or not hook_lib.is_file():
        raise FileNotFoundError(
            f"missing LD_PRELOAD library: {hook_lib}. "
            f"Build it with scripts/internal/hooks/build.sh {adapter.name}."
        )
    env["LD_PRELOAD"] = str(hook_lib)
    env["HOOK_TRACE_OUTPUT"] = str(
        resolve_run_path(ld_preload.get("trace_output", "trace/ld_preload/cpu_trace.json"), layout.run_dir)
    )


def build_bench_env(
    cfg: dict[str, Any],
    server_cfg: dict[str, Any],
    server_command: list[str] | str,
    server_env: dict[str, str],
    layout: RunLayout,
) -> dict[str, str]:
    """Build the workload environment after removing server-only instrumentation."""

    bench_env = server_env.copy()
    for key in BENCH_ENV_REMOVE_KEYS:
        bench_env.pop(key, None)
    remove_pythonpath_entry(bench_env, PYTHON_PROBE_ROOT)

    metadata = cfg.get("metadata") if isinstance(cfg.get("metadata"), dict) else {}
    bench_env["TRACE_SIM_PROFILE_RUN_DIR"] = str(layout.run_dir)
    bench_env["TRACE_SIM_PROFILE_RUN_ID"] = str(cfg.get("run_id") or cfg.get("id") or cfg.get("name") or "")
    bench_env["TRACE_SIM_PROFILE_MANIFEST_PATH"] = str(layout.run_dir / "profile_manifest.json")
    if metadata.get("suite_server_id"):
        bench_env["TRACE_SIM_PROFILE_CONFIG_ID"] = str(metadata["suite_server_id"])
    if metadata.get("suite_input_id"):
        bench_env["TRACE_SIM_PROFILE_INPUT_ID"] = str(metadata["suite_input_id"])

    model_path = cfg.get("model_path") or server_cfg.get("model_path") or parse_model_path(server_command)
    if model_path:
        bench_env["TRACE_SIM_PROFILE_MODEL_PATH"] = str(model_path)
    return bench_env


def prepend_pythonpath(env: dict[str, str], path: Path) -> None:
    """Prepend the probe root so Python discovers its ``sitecustomize`` module."""

    current = env.get("PYTHONPATH")
    env["PYTHONPATH"] = str(path) + (os.pathsep + current if current else "")


def remove_pythonpath_entry(env: dict[str, str], path: Path) -> None:
    """Remove the injected probe root so the benchmark client is not instrumented."""

    current = env.get("PYTHONPATH")
    if not current:
        return
    filtered = [item for item in current.split(os.pathsep) if item != str(path)]
    if filtered:
        env["PYTHONPATH"] = os.pathsep.join(filtered)
    else:
        env.pop("PYTHONPATH", None)
