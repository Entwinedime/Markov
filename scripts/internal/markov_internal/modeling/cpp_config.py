"""C++ TraceGraph model config 辅助工具。"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from ..common.io import load_json, write_json
from ..common.paths import ROOT_DIR, resolve_repo_path
from markov_internal.modeling_workflow.validations.hicache.oracle.evidence.capacity import (
    normalize_policy_value,
    optional_int,
)


def write_cpp_model_config(config: dict[str, Any], output_dir: Path, mode: str) -> Path | None:
    """把上层 modeling config 收敛为 C++ TraceGraph 消费的窄 model config。"""

    if mode == "faithful_replay":
        return None

    cpp_cfg = config.get("cpp_model_config")
    if isinstance(cpp_cfg, str):
        return required_repo_path(cpp_cfg)
    if isinstance(cpp_cfg, dict):
        path = output_dir / "cpp_model_config.json"
        write_json(path, cpp_cfg)
        return path

    node_scale = node_scale_config_from_modules(config)
    experiment_hicache = hicache_config_from_target_experiment(config)
    hicache = hicache_config_from_modules(config)
    modules: list[str] = []
    generated: dict[str, Any] = {"modules": modules}
    if node_scale is not None:
        modules.append("node_scale")
        generated["node_scale"] = node_scale
    if experiment_hicache is not None or hicache is not None:
        modules.append("hicache")
        merged_hicache: dict[str, Any] = {"enabled": True}
        if experiment_hicache is not None:
            merged_hicache.update(experiment_hicache)
        if hicache is not None:
            merged_hicache.update(hicache)
        generated["hicache"] = merged_hicache
    if not modules:
        return None
    path = output_dir / "cpp_model_config.json"
    write_json(path, generated)
    return path


def node_scale_config_from_modules(config: dict[str, Any]) -> dict[str, Any] | None:
    """从 modules 配置中提取 NodeScaleModule 的窄配置。"""

    for module in config.get("modules") or []:
        if not isinstance(module, dict) or not module.get("enabled", True):
            continue
        name = str(module.get("name") or "").replace("-", "_").lower()
        if name not in {"nodescalemodule", "node_scale", "nodescale", "scale"}:
            continue
        module_cfg = module.get("config") if isinstance(module.get("config"), dict) else {}
        rules: list[dict[str, Any]] = []
        for rule in module_cfg.get("rules") or []:
            if not isinstance(rule, dict):
                continue
            node_name = rule.get("name")
            factor = rule.get("factor", rule.get("scale"))
            if isinstance(node_name, str) and node_name and factor is not None:
                row = {"name": node_name, "factor": factor}
                if isinstance(rule.get("id"), str):
                    row["id"] = rule["id"]
                rules.append(row)
        return {"enabled": True, "rules": rules}
    return None


def hicache_config_from_modules(config: dict[str, Any]) -> dict[str, Any] | None:
    """从 modules 配置中提取 HiCacheModule 的显式 target config。"""

    for module in config.get("modules") or []:
        if not isinstance(module, dict) or not module.get("enabled", True):
            continue
        name = str(module.get("name") or "").replace("-", "_").lower()
        if name not in {"hicachemodule", "hicache"}:
            continue
        module_cfg = dict(module.get("config") or {})
        hicache = dict(module_cfg.get("hicache") or module_cfg)
        result: dict[str, Any] = {"enabled": bool(hicache.get("enabled", True))}
        copy_hicache_config_keys(result, hicache, source_label="HiCache")
        return result
    return None


def hicache_config_from_target_experiment(config: dict[str, Any]) -> dict[str, Any] | None:
    """从目标实验配置中抽取 HiCache target config。"""

    input_cfg = config.get("input") if isinstance(config.get("input"), dict) else {}
    raw_experiment = input_cfg.get("target_experiment_config", input_cfg.get("target_experiment_config_path"))
    if raw_experiment is None:
        return None

    if isinstance(raw_experiment, str):
        experiment = load_json(required_repo_path(raw_experiment))
    elif isinstance(raw_experiment, dict):
        experiment = raw_experiment
    else:
        raise TypeError("input.target_experiment_config must be a path or an object")

    result: dict[str, Any] = {"enabled": True}
    command_cfg = experiment.get("server") if isinstance(experiment.get("server"), dict) else {}
    command = command_cfg.get("command")
    if isinstance(command, list):
        command_values = [str(item) for item in command]
        _copy_command_int_option(result, command_values, "--page-size", "page_size")
        _copy_command_policy_option(result, command_values, "--hicache-write-policy", "write_policy")
        _copy_command_policy_option(result, command_values, "--hicache-storage-prefetch-policy", "prefetch_policy")
        extra = _command_option_value(command_values, "--hicache-storage-backend-extra-config")
        if extra:
            _copy_hicache_storage_extra_config(result, extra)

    modeling_cfg = experiment.get("modeling") if isinstance(experiment.get("modeling"), dict) else {}
    explicit_hicache = modeling_cfg.get("hicache") if isinstance(modeling_cfg.get("hicache"), dict) else {}
    copy_hicache_config_keys(result, explicit_hicache, source_label="target experiment modeling.hicache")
    return result if len(result) > 1 else None


def copy_hicache_config_keys(result: dict[str, Any], hicache: dict[str, Any], *, source_label: str) -> None:
    """把显式 HiCache config key 复制到 C++ model config payload。"""

    for key in (
        "page_size",
        "l1_capacity_pages",
        "l1_capacity",
        "l2_capacity_pages",
        "l2_capacity",
        "write_policy",
        "write_through_threshold",
        "prefetch_policy",
        "prefetch_threshold_pages",
        "prefetch_capacity_limit_pages",
        "prefetch_timeout_base_sec",
        "prefetch_timeout_per_ki_token_sec",
        "prefetch_timeout_max_sec",
        "prefetch_timeout_base",
        "prefetch_timeout_per_ki_token",
        "prefetch_timeout_max",
        "device_allocator_need_sort",
        "disaggregation_mode",
        "emit_state_digests",
    ):
        if key not in hicache:
            continue
        if key in {"write_policy", "prefetch_policy"}:
            value = normalize_policy_value(hicache.get(key))
            if value == "observed":
                raise ValueError(f"{source_label} {key}=observed is not supported; use an explicit target policy")
            if value:
                result[key] = value
            continue
        result[key] = hicache[key]


def _command_option_value(command: list[str], option: str) -> str | None:
    """读取 `--name value` 或 `--name=value` 格式的命令行参数。"""

    prefix = option + "="
    for index, item in enumerate(command):
        if item == option and index + 1 < len(command):
            return command[index + 1]
        if item.startswith(prefix):
            return item[len(prefix) :]
    return None


def _copy_command_int_option(result: dict[str, Any], command: list[str], option: str, key: str) -> None:
    """从 target experiment command 中复制整数型 HiCache 选项。"""

    raw = _command_option_value(command, option)
    value = optional_int(raw)
    if value is not None and value > 0:
        result[key] = value


def _copy_command_policy_option(result: dict[str, Any], command: list[str], option: str, key: str) -> None:
    """从 target experiment command 中复制 policy 字符串选项。"""

    raw = _command_option_value(command, option)
    if raw is None:
        return
    value = normalize_policy_value(raw)
    if value == "observed":
        raise ValueError(f"{option}=observed is not supported; use an explicit target policy")
    if value:
        result[key] = value


def _copy_hicache_storage_extra_config(result: dict[str, Any], raw_extra: str) -> None:
    """抽取 storage extra config 中 C++ state model 已消费的字段。"""

    try:
        extra = json.loads(raw_extra)
    except json.JSONDecodeError:
        return
    if not isinstance(extra, dict):
        return
    mapping = {
        "prefetch_timeout_base": "prefetch_timeout_base_sec",
        "prefetch_timeout_per_ki_token": "prefetch_timeout_per_ki_token_sec",
        "prefetch_timeout_max": "prefetch_timeout_max_sec",
        "prefetch_threshold_pages": "prefetch_threshold_pages",
        "prefetch_capacity_limit_pages": "prefetch_capacity_limit_pages",
    }
    for src, dst in mapping.items():
        if src in extra:
            result[dst] = extra[src]


def trace_graph_executable(config: dict[str, Any]) -> Path:
    """解析 C++ trace_graph 可执行文件路径。"""

    cpp = config.get("cpp_trace_graph") if isinstance(config.get("cpp_trace_graph"), dict) else {}
    backend_kind = str(cpp.get("backend_kind") or "").strip().lower()
    if backend_kind == "validation" or cpp.get("require_debug") is True:
        executable = ROOT_DIR / "build/modeling/trace_graph-debug/trace_graph"
        if executable.is_file():
            return executable
        raise FileNotFoundError(
            "missing validation trace_graph executable at build/modeling/trace_graph-debug/trace_graph; "
            "run scripts/run.sh modeling -- bash -lc 'cmake -S src/modeling/trace_graph "
            "-B build/modeling/trace_graph-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DTRACE_GRAPH_DEBUG=ON "
            "&& cmake --build build/modeling/trace_graph-debug --target trace_graph -j2'"
        )
    if isinstance(cpp.get("executable"), str):
        executable = required_repo_path(cpp["executable"])
        if executable.is_file():
            return executable
    executable = ROOT_DIR / "build/modeling/trace_graph-release/trace_graph"
    if executable.is_file():
        return executable
    raise FileNotFoundError(
        "missing release trace_graph executable at build/modeling/trace_graph-release/trace_graph; "
        "run scripts/run.sh modeling -- bash -lc 'cmake -S src/modeling/trace_graph "
        "-B build/modeling/trace_graph-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRACE_GRAPH_DEBUG=OFF "
        "&& cmake --build build/modeling/trace_graph-release --target trace_graph -j2'"
    )


def required_repo_path(value: Any) -> Path:
    """解析必填 repo path，缺失时抛出错误。"""

    path = resolve_repo_path(value)
    if path is None:
        raise ValueError("expected a non-empty path")
    return path
