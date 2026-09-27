"""Environment construction for profiled server and workload processes."""

from __future__ import annotations

import json
import os
from typing import TYPE_CHECKING, Any

from ..common.manifest import profile_labels
from ..common.paths import ROOT_DIR, resolve_repo_path
from .frameworks import FrameworkAdapter
from .runtime import (
    RunLayout,
    expand_layout_placeholders,
)

if TYPE_CHECKING:
    from profiling.config import ProfilingRuntimeConfig

PYTHON_PROBE_ROOT = ROOT_DIR / "src/profiling/python_probe"
PYTHON_PROBE_ENV_KEYS = (
    "TRACE_SIM_PYTHON_PROBE",
    "TRACE_SIM_PYTHON_PROBE_TARGETS",
    "TRACE_SIM_PYTHON_PROBE_OUTPUT",
    "TRACE_SIM_PYTHON_PROBE_DEBUG",
    "TRACE_SIM_PYTHON_PROBE_DIAGNOSTICS",
    "TRACE_SIM_PYTHON_PROBE_CONSUMERS",
    "TRACE_SIM_PYTHON_PROBE_FLUSH_EVERY",
    "TRACE_SIM_PYTHON_PROBE_FLUSH_INTERVAL_SEC",
)


def build_server_env(
    cfg: dict[str, Any],
    runtime: ProfilingRuntimeConfig,
    layout: RunLayout,
    adapter: FrameworkAdapter,
    python_targets: list[dict[str, Any]],
) -> dict[str, str]:
    """Build the server environment with only enabled capture channels."""

    env = os.environ.copy()
    for key, value in cfg.get("env", {}).items():
        env[str(key)] = expand_layout_placeholders(str(value), layout, cfg)

    # Probe activation belongs to this run's channel configuration, not the parent process.
    _remove_python_probe_env(env)
    apply_framework_defaults(env, adapter)
    if adapter.profiler_api:
        env["SGLANG_TORCH_PROFILER_DIR"] = str(runtime.output_path(layout.run_dir, "torch"))
    env["TRACE_SIM_PROFILING_CHANNELS"] = ",".join(runtime.channels)

    if runtime.enabled and "python_probe" in runtime.channels:
        apply_python_probe_env(env, runtime, layout, python_targets)
    if runtime.enabled and "ld_preload" in runtime.channels:
        apply_ld_preload_env(env, runtime, layout, adapter)
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


def apply_python_probe_env(
    env: dict[str, str], runtime: ProfilingRuntimeConfig, layout: RunLayout, python_targets: list[dict[str, Any]]
) -> None:
    """Inject Python probe activation, target contracts, and output paths."""

    current_pythonpath = env.get("PYTHONPATH")
    env["PYTHONPATH"] = str(PYTHON_PROBE_ROOT) + (os.pathsep + current_pythonpath if current_pythonpath else "")

    env["TRACE_SIM_PYTHON_PROBE"] = "1"
    env["TRACE_SIM_PYTHON_PROBE_DIAGNOSTICS"] = runtime.python_diagnostics
    env["TRACE_SIM_PYTHON_PROBE_CONSUMERS"] = ",".join(runtime.python_consumers)
    env["TRACE_SIM_PYTHON_PROBE_TARGETS"] = json.dumps(python_targets, ensure_ascii=False)
    env["TRACE_SIM_PYTHON_PROBE_OUTPUT"] = str(layout.trace_dir / "python_probe")
    flush_every = runtime.channel_options["python_probe"].get("flush_every")
    if flush_every is not None:
        env["TRACE_SIM_PYTHON_PROBE_FLUSH_EVERY"] = str(flush_every)
    flush_interval_sec = runtime.python_probe_flush_interval_sec
    if flush_interval_sec > 0:
        env["TRACE_SIM_PYTHON_PROBE_FLUSH_INTERVAL_SEC"] = f"{flush_interval_sec:g}"
    if runtime.debug:
        env["TRACE_SIM_PYTHON_PROBE_DEBUG"] = "1"


def apply_ld_preload_env(
    env: dict[str, str], runtime: ProfilingRuntimeConfig, layout: RunLayout, adapter: FrameworkAdapter
) -> None:
    """Inject the LD_PRELOAD hook and bind its output to the current run."""

    ld_preload = runtime.channel_options["ld_preload"]
    if not ld_preload.get("enabled", True):
        return
    hook_lib = resolve_repo_path(ld_preload.get("library", adapter.hook_library))
    if hook_lib is None or not hook_lib.is_file():
        raise FileNotFoundError(
            f"missing LD_PRELOAD library: {hook_lib}. Build it with scripts/internal/hooks/build.sh {adapter.name}."
        )
    env["LD_PRELOAD"] = str(hook_lib)
    output = runtime.output_path(layout.run_dir, "ld_preload")
    output.parent.mkdir(parents=True, exist_ok=True)
    env["HOOK_TRACE_OUTPUT"] = str(output)


def build_bench_env(
    cfg: dict[str, Any],
    server_env: dict[str, str],
    layout: RunLayout,
    model_path: str | None,
) -> dict[str, str]:
    """Build the workload environment after removing server-only instrumentation."""

    bench_env = server_env.copy()
    for key in ("LD_PRELOAD", "HOOK_TRACE_OUTPUT"):
        bench_env.pop(key, None)
    _remove_python_probe_env(bench_env)

    config_id, input_id = profile_labels(cfg)
    bench_env["TRACE_SIM_PROFILE_RUN_DIR"] = str(layout.run_dir)
    bench_env["TRACE_SIM_PROFILE_RUN_ID"] = str(cfg.get("run_id") or cfg.get("id") or cfg.get("name") or "")
    bench_env["TRACE_SIM_PROFILE_MANIFEST_PATH"] = str(layout.run_dir / "profile_manifest.json")
    for suffix, value in (
        ("CONFIG_ID", config_id),
        ("INPUT_ID", input_id),
        ("MODEL_PATH", model_path),
    ):
        key = "TRACE_SIM_PROFILE_" + suffix
        if value:
            bench_env[key] = str(value)
        else:
            bench_env.pop(key, None)
    return bench_env


def _remove_python_probe_env(env: dict[str, str]) -> None:
    """Clear project probe injection while retaining unrelated Python search paths."""

    for key in PYTHON_PROBE_ENV_KEYS:
        env.pop(key, None)

    current = env.get("PYTHONPATH")
    if not current:
        return
    filtered = [item for item in current.split(os.pathsep) if item != str(PYTHON_PROBE_ROOT)]
    if filtered:
        env["PYTHONPATH"] = os.pathsep.join(filtered)
    else:
        env.pop("PYTHONPATH", None)
