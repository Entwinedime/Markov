"""Profiling process environment builders."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

from ..common.paths import ROOT_DIR, resolve_repo_path
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
    "TRACE_SIM_PYTHON_PROBES",
    "TRACE_SIM_PYTHON_PROBE_TARGETS",
    "TRACE_SIM_PYTHON_PROBE_OUTPUT",
    "TRACE_SIM_PYTHON_PROBE_DEBUG",
    "TRACE_SIM_PYTHON_PROBE_FLUSH_EVERY",
    "TRACE_SIM_HICACHE_CONSUMERS",
    "TRACE_SIM_HICACHE_INTERNAL_HOOKS",
)


def build_server_env(cfg: dict[str, Any], runtime: Any, layout: RunLayout) -> dict[str, str]:
    """构造 server 环境。"""

    env = os.environ.copy()
    for key, value in cfg.get("env", {}).items():
        env[str(key)] = expand_layout_placeholders(str(value), layout, cfg)

    apply_sglang_defaults(env)
    env["SGLANG_TORCH_PROFILER_DIR"] = str(layout.torch_trace_dir)
    env["TRACE_SIM_PROFILING_CHANNELS"] = ",".join(runtime.channels)
    env["TRACE_SIM_PROFILING_DEBUG"] = "1" if runtime.debug else "0"

    if runtime.enabled and "python_probe" in runtime.channels:
        apply_python_probe_env(env, cfg, runtime, layout)
    if runtime.enabled and "ld_preload" in runtime.channels:
        apply_ld_preload_env(env, cfg, layout)
    return env


def apply_sglang_defaults(env: dict[str, str]) -> None:
    """写入 SGLang / Ascend 常用默认值，但不覆盖用户显式 env。"""

    env.setdefault("HOOK_ASCENDCL_SO_PATH", "/usr/local/Ascend/ascend-toolkit/latest/lib64/libascendcl.so")
    env.setdefault("SGLANG_SET_CPU_AFFINITY", "1")
    env.setdefault("PYTORCH_NPU_ALLOC_CONF", "expandable_segments:True")
    env.setdefault("STREAMS_PER_DEVICE", "32")
    env.setdefault("HCCL_BUFFSIZE", "1536")
    env.setdefault("HCCL_OP_EXPANSION_MODE", "AIV")


def apply_python_probe_env(env: dict[str, str], cfg: dict[str, Any], runtime: Any, layout: RunLayout) -> None:
    """注入 Python probe 环境变量和 target 配置。"""

    python_probe = channel_config(cfg, "python_probe")
    selected_targets = select_python_probe_targets(
        runtime.python_target_catalog,
        tuple(runtime.python_consumers),
    )
    prepend_pythonpath(env, PYTHON_PROBE_ROOT)
    env["TRACE_SIM_PYTHON_PROBE"] = "1"
    env["TRACE_SIM_PYTHON_PROBES"] = ",".join(runtime.python_probes)
    env["TRACE_SIM_PYTHON_PROBE_TARGETS"] = json.dumps(selected_targets, ensure_ascii=False)
    env["TRACE_SIM_PYTHON_PROBE_OUTPUT"] = str(layout.trace_dir / "python_probe")
    env["TRACE_SIM_HICACHE_CONSUMERS"] = ",".join(runtime.python_consumers)
    flush_every = python_probe.get("flush_every", python_probe.get("flush_interval_events"))
    if flush_every is not None:
        env["TRACE_SIM_PYTHON_PROBE_FLUSH_EVERY"] = str(flush_every)
    if "internal_hooks" in python_probe:
        env["TRACE_SIM_HICACHE_INTERNAL_HOOKS"] = "1" if bool(python_probe.get("internal_hooks")) else "0"
    if runtime.debug:
        env["TRACE_SIM_PYTHON_PROBE_DEBUG"] = "1"


def apply_ld_preload_env(env: dict[str, str], cfg: dict[str, Any], layout: RunLayout) -> None:
    """注入 LD_PRELOAD hook，并把输出固定到本次 run 目录。"""

    ld_preload = channel_config(cfg, "ld_preload")
    if not ld_preload.get("enabled", True):
        return
    hook_lib = resolve_repo_path(ld_preload.get("library", "build/docker/sglang/lib/libhook.so"))
    if hook_lib is None or not hook_lib.is_file():
        raise FileNotFoundError(
            f"missing LD_PRELOAD library: {hook_lib}. Build it with scripts/internal/hooks/build.sh sglang."
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
    """构造 workload 环境，并移除只应注入 server 的采集变量。"""

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

    forced_token_bundle = metadata.get("forced_token_bundle")
    if isinstance(forced_token_bundle, dict):
        bundle_env = {
            "TRACE_SIM_FORCED_TOKEN_BUNDLE_PATH": forced_token_bundle.get("path"),
            "TRACE_SIM_FORCED_TOKEN_BUNDLE_SCHEMA": forced_token_bundle.get("schema"),
            "TRACE_SIM_FORCED_TOKEN_BUNDLE_SHA256": forced_token_bundle.get("sha256"),
            "TRACE_SIM_FORCED_TOKEN_BUNDLE_ID": forced_token_bundle.get("bundle_id"),
            "TRACE_SIM_FORCED_TOKEN_BUNDLE_PLAN_SHA256": forced_token_bundle.get("plan_sha256"),
        }
        for key, value in bundle_env.items():
            if value:
                bench_env[key] = str(value)

    model_path = cfg.get("model_path") or server_cfg.get("model_path") or parse_model_path(server_command)
    if model_path:
        bench_env["TRACE_SIM_PROFILE_MODEL_PATH"] = str(model_path)
    return bench_env


def prepend_pythonpath(env: dict[str, str], path: Path) -> None:
    """把 probe 路径放到 PYTHONPATH 前面，确保 sitecustomize 可以被 Python 发现。"""

    current = env.get("PYTHONPATH")
    env["PYTHONPATH"] = str(path) + (os.pathsep + current if current else "")


def remove_pythonpath_entry(env: dict[str, str], path: Path) -> None:
    """从 PYTHONPATH 移除 runner 注入项，避免 bench client 被误插桩。"""

    current = env.get("PYTHONPATH")
    if not current:
        return
    filtered = [item for item in current.split(os.pathsep) if item != str(path)]
    if filtered:
        env["PYTHONPATH"] = os.pathsep.join(filtered)
    else:
        env.pop("PYTHONPATH", None)
