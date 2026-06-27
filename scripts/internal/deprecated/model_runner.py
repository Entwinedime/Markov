#!/usr/bin/env python3
"""Modeling CLI。

本脚本只做编排：读取配置、调用 trace merger、启动 C++ TraceGraph 后端。
建模逻辑、DAG 构建、拓扑仿真和子模块执行都在 C++ 中完成。
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROOT_DIR = Path(__file__).resolve().parents[2]
CONTAINER_REPO_PREFIXES = ("/workspace/trace-sim", "/opt/trace-sim")
MODELING_CONTAINER_ENV = "TRACE_SIM_MODELING_CONTAINER"
SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from hicache_fact_contract import parse_fact_or_none  # noqa: E402
from trace_json import load_chrome_trace_events  # noqa: E402


@dataclass(frozen=True)
class WorkloadWindow:
    """workload 真实耗时窗口。"""

    report_path: Path
    start_ns: int
    end_ns: int
    actual_e2e_ns: int
    source: str = "workload_report"


@dataclass(frozen=True)
class ModelingOptions:
    """命令行参数。"""

    config_path: Path
    output_dir: Path | None
    profile_manifest: Path | None
    cpp_model_config: Path | None
    hicache_oracle_traces: list[Path]
    mode: str | None
    emit_dag_chrome_trace: bool
    emit_module_summary: bool
    emit_validation: bool
    debug: bool


def load_json(path: Path) -> dict[str, Any]:
    """读取 JSON 对象配置或运行摘要。"""

    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value: Any) -> None:
    """以稳定缩进写出 JSON，并创建父目录。"""

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def resolve_repo_path(value: str) -> Path:
    """把用户输入路径解析为仓库内真实路径。"""

    path = Path(value).expanduser()
    if path.is_absolute():
        return map_repo_path(path)
    return ROOT_DIR / path


def map_repo_path(path: Path) -> Path:
    """把容器内仓库前缀映射成本机仓库路径。"""

    raw = str(path)
    for prefix in CONTAINER_REPO_PREFIXES:
        if raw == prefix:
            return ROOT_DIR
        if raw.startswith(prefix + "/"):
            return ROOT_DIR / raw[len(prefix) + 1 :]
    return path


def running_in_modeling_container() -> bool:
    """判断当前 runner 是否位于受支持的 modeling Docker 环境。"""

    root = str(ROOT_DIR)
    under_container_repo = any(root == prefix or root.startswith(prefix + "/") for prefix in CONTAINER_REPO_PREFIXES)
    has_container_marker = os.environ.get(MODELING_CONTAINER_ENV) == "1" or Path("/.dockerenv").exists()
    return under_container_repo and has_container_marker


def require_modeling_container() -> None:
    """阻止宿主机直接执行 modeling runner。"""

    if running_in_modeling_container():
        return
    raise SystemExit(
        "scripts/internal/model_runner.py is container-internal. "
        "Use scripts/model.sh ... or scripts/run.sh modeling -- ... instead."
    )


def parse_args(argv: list[str] | None = None) -> ModelingOptions:
    """解析 modeling runner CLI 参数。"""

    parser = argparse.ArgumentParser(description="Run C++ trace-based modeling.")
    parser.add_argument("--config", required=True, help="modeling config path")
    parser.add_argument("--output-dir", help="override config.output_dir")
    parser.add_argument("--profile-manifest", help="override config.input.profile_manifest")
    parser.add_argument("--cpp-model-config", help="override config.cpp_model_config with a ready-to-use C++ model config JSON")
    parser.add_argument(
        "--hicache-oracle-trace",
        action="append",
        default=[],
        help="override validation.hicache_state.oracle_trace_paths; may be repeated",
    )
    parser.add_argument("--mode", choices=("faithful_replay", "cache_state", "cache_patch"), help="override config.mode")
    parser.add_argument("--emit-dag-chrome-trace", action="store_true", help="emit DAG as Chrome trace JSON")
    parser.add_argument("--emit-module-summary", action="store_true", help="emit C++ module summary JSON")
    parser.add_argument("--emit-validation", action="store_true", help="emit validation.json")
    parser.add_argument("--debug", action="store_true", help="enable C++ TraceGraph debug mode")
    args = parser.parse_args(argv)

    config_path = resolve_repo_path(args.config)
    if not config_path.is_file():
        raise FileNotFoundError(f"missing config: {config_path}")
    return ModelingOptions(
        config_path=config_path,
        output_dir=resolve_repo_path(args.output_dir) if args.output_dir else None,
        profile_manifest=resolve_repo_path(args.profile_manifest) if args.profile_manifest else None,
        cpp_model_config=resolve_repo_path(args.cpp_model_config) if args.cpp_model_config else None,
        hicache_oracle_traces=[resolve_repo_path(path) for path in args.hicache_oracle_trace],
        mode=args.mode,
        emit_dag_chrome_trace=bool(args.emit_dag_chrome_trace),
        emit_module_summary=bool(args.emit_module_summary),
        emit_validation=bool(args.emit_validation),
        debug=bool(args.debug),
    )


def run_from_cli(options: ModelingOptions) -> dict[str, Any]:
    """执行一次 modeling run。

    Python 侧只准备输入 trace、C++ model config 和输出路径；实际 DAG 构建、模块执行和拓扑仿真
    均由 C++ TraceGraph 完成。
    """

    config = load_json(options.config_path)
    mode = options.mode or str(config.get("mode") or "faithful_replay")
    output_dir = options.output_dir or resolve_repo_path(str(config.get("output_dir") or "data/modeling_runs/cpp_trace_graph"))
    output_dir.mkdir(parents=True, exist_ok=True)
    outputs_cfg = config.get("outputs") if isinstance(config.get("outputs"), dict) else {}
    emit_dag_chrome_trace = options.emit_dag_chrome_trace or bool(outputs_cfg.get("emit_dag_chrome_trace", False))
    emit_module_summary = options.emit_module_summary or bool(outputs_cfg.get("emit_module_summary", False))
    emit_validation = options.emit_validation or bool(outputs_cfg.get("emit_validation", False))
    if emit_validation and hicache_state_validation_enabled(config):
        # HiCache state validation 依赖 C++ module summary 中的 state trace。
        emit_module_summary = True
    debug = options.debug or bool(outputs_cfg.get("debug", False))

    input_cfg = config.get("input") if isinstance(config.get("input"), dict) else {}
    manifest_path = options.profile_manifest
    if manifest_path is None and isinstance(input_cfg.get("profile_manifest"), str):
        manifest_path = resolve_repo_path(input_cfg["profile_manifest"])

    trace_paths = prepare_trace_inputs(config, input_cfg, manifest_path, output_dir)
    model_config_path = options.cpp_model_config or write_cpp_model_config(config, output_dir, mode)
    if model_config_path is not None and not model_config_path.is_file():
        raise FileNotFoundError(f"missing C++ model config: {model_config_path}")
    workload_window = discover_workload_window(input_cfg, manifest_path)

    graph_output = output_dir / "dag_chrome_trace.json"
    run_summary = output_dir / "run_summary.json"
    module_summary = output_dir / "model_summary.json"
    command = [
        str(trace_graph_executable(config)),
        "--run-summary",
        str(run_summary),
        "--scenario-name",
        mode,
    ]
    for path in trace_paths:
        command.extend(["--input", str(path)])
    if debug:
        command.append("--debug")
    if emit_dag_chrome_trace:
        command.extend(["--graph-output", str(graph_output), "--full-output"])
    if emit_module_summary:
        command.extend(["--model-summary", str(module_summary)])
    if model_config_path is not None:
        command.extend(["--model-config", str(model_config_path)])

    completed = subprocess.run(command, cwd=ROOT_DIR, text=True, capture_output=True, check=False)
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip() or "<no stdout/stderr>"
        raise RuntimeError(
            "C++ TraceGraph failed "
            f"(returncode={completed.returncode}, command={json.dumps(command, ensure_ascii=False)}): {detail}"
        )

    summary = load_json(run_summary)
    prediction = {"predicted_e2e_ns": int(summary.get("simulated_e2e_ns", 0))}
    write_json(output_dir / "prediction.json", prediction)
    predicted_state_trace = write_hicache_predicted_state_trace_if_available(module_summary, output_dir)
    if emit_validation:
        validation = build_validation(
            mode,
            prediction,
            summary,
            workload_window,
            trace_paths,
            config,
            module_summary,
            predicted_state_trace,
            options.hicache_oracle_traces,
        )
        recommended_config_path = write_hicache_recommended_cpp_config_if_available(validation, output_dir)
        if recommended_config_path is not None and isinstance(validation.get("hicache_state"), dict):
            validation["hicache_state"]["recommended_hicache_cpp_model_config_path"] = str(recommended_config_path)
        write_json(output_dir / "validation.json", validation)
    return prediction


def prepare_trace_inputs(config: dict[str, Any], input_cfg: dict[str, Any], manifest_path: Path | None, output_dir: Path) -> list[Path]:
    """根据 config 或 profile manifest 准备 C++ 后端输入 trace。"""

    if manifest_path is not None:
        merged_dir = output_dir / "merged_trace"
        reusable = load_reusable_merge_summary(merged_dir, manifest_path)
        if reusable:
            return reusable
        command = [
            sys.executable,
            str(ROOT_DIR / "scripts/trace/trace_merger.py"),
            "--manifest",
            str(manifest_path),
            "--out-dir",
            str(merged_dir),
        ]
        merge_cfg = config.get("trace_merge") if isinstance(config.get("trace_merge"), dict) else {}
        if "tolerance_us" in merge_cfg:
            command.extend(["--tolerance", str(merge_cfg["tolerance_us"])])
        if "search_window" in merge_cfg:
            command.extend(["--window", str(merge_cfg["search_window"])])
        if "margin_us" in merge_cfg:
            command.extend(["--margin", str(merge_cfg["margin_us"])])
        if "mode" in merge_cfg:
            command.extend(["--mode", str(merge_cfg["mode"])])
        subprocess.run(command, cwd=ROOT_DIR, check=True)
        summary = load_json(merged_dir / "merge_manifest_summary.json")
        return [resolve_repo_path(path) for path in summary.get("merged_trace_files", [])]

    raw_paths: list[Any] = []
    raw_paths.extend(input_cfg.get("trace_paths") or [])
    paths = [resolve_repo_path(str(path)) for path in raw_paths]
    existing = [path for path in paths if path.is_file()]
    if not existing:
        raise ValueError("modeling input has no trace files")
    return existing


def load_reusable_merge_summary(merged_dir: Path, manifest_path: Path) -> list[Path]:
    """复用同一输出目录中已完成、且 manifest 匹配的 trace merger 结果。"""

    summary_path = merged_dir / "merge_manifest_summary.json"
    if not summary_path.is_file():
        return []
    try:
        summary = load_json(summary_path)
    except json.JSONDecodeError:
        return []

    raw_manifest = summary.get("manifest_path")
    if not isinstance(raw_manifest, str):
        return []
    if map_repo_path(Path(raw_manifest)) != manifest_path:
        return []

    paths = [resolve_repo_path(str(path)) for path in summary.get("merged_trace_files", [])]
    if not paths or any(not path.is_file() for path in paths):
        return []
    return paths


def write_cpp_model_config(config: dict[str, Any], output_dir: Path, mode: str) -> Path | None:
    """把上层 modeling config 收敛为 C++ TraceGraph 消费的窄 model config。"""

    if mode == "faithful_replay":
        return None

    cpp_cfg = config.get("cpp_model_config")
    if isinstance(cpp_cfg, str):
        return resolve_repo_path(cpp_cfg)
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
            if key in hicache:
                if key in {"write_policy", "prefetch_policy"}:
                    value = _normalize_policy_value(hicache.get(key))
                    if value == "observed":
                        raise ValueError(f"HiCache {key}=observed is not supported; use an explicit target policy")
                    if value:
                        result[key] = value
                    continue
                result[key] = hicache[key]
        return result
    return None


def hicache_config_from_target_experiment(config: dict[str, Any]) -> dict[str, Any] | None:
    """从目标实验配置中抽取 HiCache target config。

    该入口用于纯 what-if / state prediction：实验配置描述真实 SGLang
    启动参数，modeling 配置可以通过 `input.target_experiment_config`
    引用它。这里只抽取显式参数；capacity 不根据 token budget 粗算，
    只有实验配置 `modeling.hicache` 明确写出时才进入 C++ config。
    """

    input_cfg = config.get("input") if isinstance(config.get("input"), dict) else {}
    raw_experiment = input_cfg.get("target_experiment_config", input_cfg.get("target_experiment_config_path"))
    if raw_experiment is None:
        return None

    if isinstance(raw_experiment, str):
        experiment = load_json(resolve_repo_path(raw_experiment))
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
        if key in explicit_hicache:
            if key in {"write_policy", "prefetch_policy"}:
                value = _normalize_policy_value(explicit_hicache.get(key))
                if value == "observed":
                    raise ValueError(f"target experiment modeling.hicache {key}=observed is not supported; use an explicit target policy")
                if value:
                    result[key] = value
                continue
            result[key] = explicit_hicache[key]

    return result if len(result) > 1 else None


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
    value = _optional_int(raw)
    if value is not None and value > 0:
        result[key] = value


def _copy_command_policy_option(result: dict[str, Any], command: list[str], option: str, key: str) -> None:
    """从 target experiment command 中复制 policy 字符串选项。"""

    raw = _command_option_value(command, option)
    if raw is None:
        return
    value = _normalize_policy_value(raw)
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


def hicache_state_validation_enabled(config: dict[str, Any]) -> bool:
    """判断当前 modeling config 是否启用 HiCache state validation。"""

    validation_cfg = config.get("validation") if isinstance(config.get("validation"), dict) else {}
    hicache_cfg = validation_cfg.get("hicache_state") if isinstance(validation_cfg.get("hicache_state"), dict) else {}
    return bool(hicache_cfg.get("enabled", False))


def write_hicache_predicted_state_trace_if_available(module_summary_path: Path, output_dir: Path) -> Path | None:
    """把 C++ HiCache summary 中的 transition trace 拆成验证专用输出。

    默认业务输出仍只有 `prediction.json`。该文件只在显式 module summary /
    validation 场景下存在，用于 self-config / cross-config prediction state diff。
    """

    if not module_summary_path.is_file():
        return None
    try:
        module_summary = load_json(module_summary_path)
    except json.JSONDecodeError:
        return None
    hicache_summary = extract_hicache_summary(module_summary)
    if not hicache_summary:
        return None

    rows = []
    for row in hicache_summary.get("transition_trace", []):
        if not isinstance(row, dict):
            continue
        pages = [str(item) for item in row.get("pages", []) if item is not None]
        rows.append(
            {
                "request_id": row.get("request_id") or "",
                "operation_id": row.get("operation_id") or "",
                "source_fact_id": f"trace_event:{row.get('source_event_index', '')}",
                "source_event_index": row.get("source_event_index"),
                "source_event_name": row.get("event_name") or "",
                "cache_scope": row.get("cache_scope") or "",
                "ts": row.get("ts"),
                "event_base_name": event_base_name(str(row.get("event_name") or "")),
                "target_page_set": pages,
                "decision_kind": "state_prediction",
                "decision_reason": "derived_from_hicache_fact",
                "transition_kind": row.get("kind") or "",
                "tier_src": tier_src_from_transition(row),
                "tier_dst": tier_dst_from_transition(row),
                "before_state_digest": row.get("before_state_digest") or "",
                "after_state_digest": row.get("after_state_digest") or "",
                "predicted_operation_kind": predicted_operation_kind_from_transition(row),
                "blocking_class": "unknown",
                "unresolved_inputs": [],
            }
        )

    output_path = output_dir / "predicted_target_cache_state_trace.json"
    write_json(
        output_path,
        {
            "schema": "trace_sim.hicache.predicted_state_trace.v1",
            "source": "cpp_hicache_module",
            "record_count": len(rows),
            "records": rows,
            "final_state": hicache_summary.get("final_state", {}),
            "missing_state_model_facts": hicache_summary.get("missing_state_model_facts", {}),
            "skipped_non_state_model_events": hicache_summary.get("skipped_non_state_model_events", 0),
            "target_config": hicache_summary.get("target_config", {}),
            "dag_mutations": hicache_summary.get("dag_mutations", 0),
        },
    )
    return output_path


def tier_src_from_transition(row: dict[str, Any]) -> str:
    """把 C++ transition 行映射为 validation 使用的源 tier。"""

    kind = str(row.get("kind") or "")
    tier = str(row.get("tier") or "")
    if kind.startswith("remove_"):
        return tier
    return ""


def tier_dst_from_transition(row: dict[str, Any]) -> str:
    """把 C++ transition 行映射为 validation 使用的目标 tier。"""

    kind = str(row.get("kind") or "")
    tier = str(row.get("tier") or "")
    if kind.startswith("add_"):
        return tier
    return ""


def predicted_operation_kind_from_transition(row: dict[str, Any]) -> str:
    """把 C++ transition kind 归类为 oracle delta 对比使用的 operation kind。"""

    kind = str(row.get("kind") or "")
    if kind.startswith("add_") or kind.startswith("remove_"):
        return "resident_state_update"
    if kind.startswith("mark_") or kind.startswith("clear_"):
        return "page_metadata_update"
    return kind or "unknown"


def write_hicache_recommended_cpp_config_if_available(validation: dict[str, Any], output_dir: Path) -> Path | None:
    """把 HiCache 推荐配置写成可直接传给 C++ TraceGraph 的 model config。

    该文件只来自 validation oracle，用于后续 prediction 修复或复跑；当前 run
    不会反向使用它，避免让验证结果污染本次模型输入。
    """

    hicache_state = validation.get("hicache_state")
    if not isinstance(hicache_state, dict):
        return None
    capacity_audit = hicache_state.get("capacity_config_audit")
    if not isinstance(capacity_audit, dict):
        return None
    recommended = capacity_audit.get("recommended_target_config")
    if not isinstance(recommended, dict) or not recommended.get("ready"):
        return None
    hicache = recommended.get("hicache")
    if not isinstance(hicache, dict):
        return None
    payload = {
        "modules": ["hicache"],
        "hicache": {str(key): value for key, value in hicache.items() if value is not None},
    }
    output_path = output_dir / "recommended_hicache_cpp_model_config.json"
    write_json(output_path, payload)
    return output_path


def discover_workload_window(input_cfg: dict[str, Any], manifest_path: Path | None) -> WorkloadWindow | None:
    """从显式配置或 profile manifest 中发现 workload 真实时间窗。"""

    explicit = input_cfg.get("workload_report")
    if isinstance(explicit, str):
        return load_workload_window(resolve_repo_path(explicit))
    if manifest_path is None or not manifest_path.is_file():
        return None
    manifest = load_json(manifest_path)
    run_dir_raw = manifest.get("run_dir")
    run_dir = map_repo_path(Path(str(run_dir_raw))) if isinstance(run_dir_raw, str) else manifest_path.parent
    candidates = sorted(run_dir.glob("bench/**/workload_report.json"))
    if candidates:
        return load_workload_window(candidates[-1])
    bench_candidates = sorted(path for path in run_dir.glob("bench/**/*.jsonl") if path.name != "workload_report.jsonl")
    for path in reversed(bench_candidates):
        window = load_bench_serving_window(path)
        if window is not None:
            return window
    return None


def load_workload_window(path: Path) -> WorkloadWindow | None:
    """从 workload_report.json 读取请求开始/结束时间窗。"""

    if not path.is_file():
        return None
    report = load_json(path)
    requests = report.get("requests")
    if not isinstance(requests, list):
        return None
    starts: list[int] = []
    ends: list[int] = []
    for row in requests:
        if not isinstance(row, dict):
            continue
        start = optional_float(row.get("start_time_ms"))
        end = optional_float(row.get("end_time_ms"))
        if start is None or end is None:
            continue
        starts.append(int(start * 1_000_000))
        ends.append(int(end * 1_000_000))
    if not starts or not ends:
        return None
    return WorkloadWindow(path, min(starts), max(ends), max(ends) - min(starts), "workload_report")


def load_bench_serving_window(path: Path) -> WorkloadWindow | None:
    """从 SGLang bench serving JSONL 读取整体 duration。"""

    if not path.is_file():
        return None
    last: dict[str, Any] | None = None
    with path.open("r", encoding="utf-8") as file_obj:
        for line in file_obj:
            line = line.strip()
            if not line:
                continue
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict):
                last = value
    if last is None:
        return None

    duration_s = optional_float(last.get("duration"))
    if duration_s is None or duration_s <= 0:
        return None
    actual = int(duration_s * 1_000_000_000)
    return WorkloadWindow(path, 0, actual, actual, "sglang_bench_serving_duration")


def optional_float(value: Any) -> float | None:
    """宽松解析 float，失败时返回 None。"""

    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def trace_graph_executable(config: dict[str, Any]) -> Path:
    """解析 C++ trace_graph 可执行文件路径。"""

    cpp = config.get("cpp_trace_graph") if isinstance(config.get("cpp_trace_graph"), dict) else {}
    if isinstance(cpp.get("executable"), str):
        executable = resolve_repo_path(cpp["executable"])
        if executable.is_file():
            return executable
    candidate_paths = (
        ROOT_DIR / "build/modeling/bin/trace_graph",
    )
    for path in candidate_paths:
        if path.is_file():
            return path
    raise FileNotFoundError(
        "missing trace_graph executable at build/modeling/bin/trace_graph; "
        "run scripts/run.sh modeling -- bash -lc 'cmake -S . -B build/modeling -G Ninja "
        "&& cmake --build build/modeling --target trace_graph -j2'"
    )


def build_validation(
    mode: str,
    prediction: dict[str, Any],
    run_summary: dict[str, Any],
    workload_window: WorkloadWindow | None,
    trace_paths: list[Path],
    config: dict[str, Any],
    module_summary_path: Path,
    predicted_state_trace_path: Path | None,
    hicache_oracle_trace_paths: list[Path] | None = None,
) -> dict[str, Any]:
    """构造 modeling validation 输出。"""

    validation_cfg = config.get("validation") if isinstance(config.get("validation"), dict) else {}
    threshold = float(validation_cfg.get("faithful_replay_full_e2e_rel_error_max", 0.05))
    summary_real = optional_float(run_summary.get("real_e2e_ns"))
    actual = int(summary_real) if summary_real and summary_real > 0 else None
    predicted = int(prediction["predicted_e2e_ns"])
    rel_error = abs(predicted - actual) / actual if actual else None
    errors: list[str] = []
    if mode == "faithful_replay" and actual is None:
        errors.append("missing_trace_real_e2e")
    if mode == "faithful_replay" and actual and rel_error is not None and rel_error > threshold:
        errors.append("faithful_replay_full_e2e_error_too_high")
    result = {
        "mode": mode,
        "engine": "cpp_trace_graph",
        "validation_ready": not errors,
        "validation_errors": errors,
        "thresholds": {"faithful_replay_full_e2e_rel_error_max": threshold},
        "trace_files": [str(path) for path in trace_paths],
        "dag": {
            "node_count": run_summary.get("node_count"),
            "edge_count": run_summary.get("edge_count"),
            "parsed_record_count": run_summary.get("parsed_record_count"),
            "edge_counts_by_kind": run_summary.get("edge_counts_by_kind"),
            "stage_timings_ms": run_summary.get("stage_timings_ms"),
            "dag_mutation_count": 0,
        },
        "workload_window": {
            "used": workload_window is not None,
            "report_path": str(workload_window.report_path) if workload_window else None,
            "source": workload_window.source if workload_window else None,
            "actual_e2e_ns": workload_window.actual_e2e_ns if workload_window else None,
        },
        "e2e": {
            "predicted_e2e_ns": predicted,
            "actual_e2e_ns": actual,
            "actual_source": "trace_real_e2e_ns" if actual is not None else None,
            "absolute_error_ns": predicted - actual if actual else None,
            "relative_error": rel_error,
        },
    }
    hicache_validation = build_hicache_state_validation_if_enabled(
        validation_cfg,
        trace_paths,
        module_summary_path,
        predicted_state_trace_path,
        hicache_oracle_trace_paths or [],
    )
    if hicache_validation is not None:
        result["hicache_state"] = hicache_validation
        if hicache_validation.get("oracle_state_validation_required", False):
            if not hicache_validation.get("state_trace_ready", False):
                errors.append("hicache_state_trace_not_ready")
            if hicache_validation.get("state_trace_ready") and hicache_validation.get("final_state_match") is False:
                errors.append("hicache_final_state_mismatch")
        if not hicache_validation.get("state_model_fact_ready", False):
            errors.append("hicache_state_model_fact_not_ready")
        result["validation_errors"] = errors
        result["validation_ready"] = not errors
    return result


def build_hicache_state_validation_if_enabled(
    validation_cfg: dict[str, Any],
    trace_paths: list[Path],
    module_summary_path: Path,
    predicted_state_trace_path: Path | None,
    oracle_trace_paths_override: list[Path] | None = None,
) -> dict[str, Any] | None:
    """构造 HiCache state validation。

    oracle snapshot 只用于 validation/debug；该函数不会修改 C++ model config，也不会把 oracle
    状态回流到预测路径。
    """

    hicache_cfg = validation_cfg.get("hicache_state") if isinstance(validation_cfg.get("hicache_state"), dict) else {}
    if not bool(hicache_cfg.get("enabled", False)):
        return None

    oracle_paths = list(oracle_trace_paths_override or [])
    if not oracle_paths:
        oracle_paths = [resolve_repo_path(str(path)) for path in hicache_cfg.get("oracle_trace_paths", []) if isinstance(path, str)]
    if not oracle_paths:
        oracle_paths = trace_paths
    oracle_required = bool(hicache_cfg.get("require_oracle_state_trace", False))
    oracle_page_key_mode = str(hicache_cfg.get("oracle_page_key_mode") or "strip_scope")

    model_summary = load_json(module_summary_path) if module_summary_path.is_file() else {}
    hicache_summary = extract_hicache_summary(model_summary)
    snapshots = extract_hicache_state_snapshots(oracle_paths)
    oracle_final = latest_derived_state(snapshots)
    capacity_oracle = extract_hicache_capacity_oracle_state(snapshots)
    oracle_observed_max_counts = observed_max_derived_state_counts(snapshots)
    model_final = hicache_summary.get("final_state") if isinstance(hicache_summary.get("final_state"), dict) else {}
    ignored_state_keys = configured_ignore_state_keys(hicache_cfg)
    raw_sets_diff = diff_hicache_sets(model_final, oracle_final)
    raw_active_sets_diff = {key: value for key, value in raw_sets_diff.items() if key not in ignored_state_keys}
    raw_ignored_sets_diff = {key: value for key, value in raw_sets_diff.items() if key in ignored_state_keys}
    normalized_model_final = normalize_hicache_state_for_oracle_compare(model_final, oracle_page_key_mode)
    normalized_oracle_final = normalize_hicache_state_for_oracle_compare(oracle_final, oracle_page_key_mode)
    all_sets_diff = diff_hicache_sets(normalized_model_final, normalized_oracle_final)
    sets_diff = {key: value for key, value in all_sets_diff.items() if key not in ignored_state_keys}
    ignored_sets_diff = {key: value for key, value in all_sets_diff.items() if key in ignored_state_keys}
    capacity_config_audit = build_hicache_capacity_config_audit(
        capacity_oracle,
        hicache_summary.get("target_config") if isinstance(hicache_summary.get("target_config"), dict) else {},
        final_state_counts(oracle_final),
        oracle_observed_max_counts,
    )
    predicted_records = load_predicted_state_records(predicted_state_trace_path)
    first_mismatch = first_hicache_mismatch(sets_diff, predicted_records)
    raw_first_mismatch = first_hicache_mismatch(raw_active_sets_diff, predicted_records)
    request_transition_coverage = build_request_transition_coverage(predicted_records, snapshots)
    transition_coverage = build_transition_coverage(predicted_records, snapshots)
    event_delta_validation = build_event_delta_validation(predicted_records, snapshots)
    timeline_delta_validation = build_timeline_delta_validation(predicted_records, snapshots)
    skipped_non_state_model = int(hicache_summary.get("skipped_non_state_model_events", 0) or 0) if hicache_summary else 0
    missing_state_model_facts = []
    missing_state_model_counts = hicache_summary.get("missing_state_model_facts", {}) if hicache_summary else {}
    if isinstance(missing_state_model_counts, dict):
        missing_state_model_facts.extend(sorted(str(key) for key, value in missing_state_model_counts.items() if int(value or 0) > 0))

    return {
        "state_trace_ready": bool(snapshots),
        "state_trace_events": len(snapshots),
        "oracle_state_validation_required": oracle_required,
        "oracle_page_key_mode": oracle_page_key_mode,
        "model_transition_events": len(hicache_summary.get("transition_trace", []) if hicache_summary else []),
        "final_state_match": None if not oracle_final else not first_mismatch,
        "raw_final_state_match": None if not oracle_final else not raw_first_mismatch,
        "sets_diff_by_tier": sets_diff,
        "raw_sets_diff_by_tier": raw_active_sets_diff,
        "ignored_state_keys": sorted(ignored_state_keys),
        "ignored_sets_diff_by_tier": ignored_sets_diff,
        "raw_ignored_sets_diff_by_tier": raw_ignored_sets_diff,
        "model_final_state_counts": final_state_counts(model_final),
        "oracle_final_state_counts": final_state_counts(oracle_final),
        "normalized_model_final_state_counts": final_state_counts(normalized_model_final),
        "normalized_oracle_final_state_counts": final_state_counts(normalized_oracle_final),
        "oracle_observed_max_state_counts": oracle_observed_max_counts,
        "unchecked_model_state_keys": unchecked_model_state_keys(normalized_model_final, normalized_oracle_final),
        "first_mismatch": first_mismatch,
        "raw_first_mismatch": raw_first_mismatch,
        "request_transition_coverage": request_transition_coverage,
        "transition_coverage": transition_coverage,
        "event_delta_validation": event_delta_validation,
        "timeline_delta_validation": timeline_delta_validation,
        "oracle_capacity_summary": capacity_oracle,
        "capacity_config_audit": capacity_config_audit,
        "skipped_non_state_model_events": skipped_non_state_model,
        "unmatched_state_trace_events": 0 if snapshots else None,
        "state_model_fact_ready": bool(hicache_summary) and not missing_state_model_facts,
        "missing_state_model_facts": missing_state_model_facts,
        "missing_state_model_fact_counts": missing_state_model_counts if isinstance(missing_state_model_counts, dict) else {},
        "oracle_trace_files": [str(path) for path in oracle_paths],
        "model_summary_ready": bool(hicache_summary),
        "predicted_state_trace_path": str(predicted_state_trace_path) if predicted_state_trace_path else None,
        "predicted_state_trace_ready": predicted_state_trace_path is not None and predicted_state_trace_path.is_file(),
    }


def configured_ignore_state_keys(hicache_cfg: dict[str, Any]) -> set[str]:
    """读取 validation-only 的 final state diff 忽略字段。

    该配置只影响 `final_state_match` 的硬门槛，不会删除原始计数，也不会改变
    C++ HiCache 模型的状态维护逻辑。它用于 page-size 等组合实验中暂时排除
    当前目标不验证、或目标 trace 身份不可安全对齐的 debug 集合。
    """

    raw = hicache_cfg.get("ignore_state_keys")
    if not isinstance(raw, list):
        return set()
    return {str(item) for item in raw if isinstance(item, str) and item}


def extract_hicache_summary(model_summary: dict[str, Any]) -> dict[str, Any]:
    """从 C++ module summary 中提取 HiCache summary。"""

    modules = model_summary.get("modules")
    if not isinstance(modules, list):
        return {}
    for module in modules:
        if isinstance(module, dict) and isinstance(module.get("hicache"), dict):
            return module["hicache"]
    return {}


def extract_hicache_state_snapshots(trace_paths: list[Path]) -> list[dict[str, Any]]:
    """从 oracle trace 中提取 validation-only HiCache state snapshot。"""

    snapshots: list[dict[str, Any]] = []
    for path in trace_paths:
        if not path.is_file():
            continue
        events, _status = load_chrome_trace_events(path, auto_repair=True)
        for event in events:
            args = event.get("args") if isinstance(event.get("args"), dict) else {}
            if not isinstance(args, dict):
                continue
            fact = parse_fact_or_none(args)
            if fact is None or fact.fact_class != "oracle_state" or fact.role != "state_snapshot":
                continue
            snapshot = args.get("state_snapshot")
            if isinstance(snapshot, dict):
                snapshots.append(
                    {
                        # C++ state model 当前在一个 DagGraph 内聚合所有进程事件；oracle 也必须先按进程取最终快照，再做集合 union。
                        "order": len(snapshots),
                        "trace_path": str(path),
                        "pid": event.get("pid"),
                        "tid": event.get("tid"),
                        "event_name": event.get("name"),
                        "source_event_name": args.get("source_event_name"),
                        "target_id": args.get("target_id"),
                        "request_id": args.get("request_id"),
                        "operation_id": args.get("operation_id"),
                        "ts": event.get("ts"),
                        "dur": event.get("dur"),
                        "object_id": snapshot.get("object_id"),
                        "state_snapshot": snapshot,
                    }
                )
    return snapshots


def extract_hicache_capacity_oracle_state(snapshots: list[dict[str, Any]]) -> dict[str, Any]:
    """汇总 oracle trace 中的 capacity/policy 快照。

    这部分只作为验证解释输出，不参与 state diff。它的用途是把真实运行中
    暴露的 L1/L2 pool 容量、可用量和 policy 参数沉淀出来，后续用于减少
    跨配置 prediction 对手工 capacity 配置的依赖。
    """

    object_id_prefix_counts: dict[str, int] = {}
    unique_values: dict[str, set[str]] = {}
    samples: list[dict[str, Any]] = []
    snapshot_count = 0
    for row in snapshots:
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
            continue
        capacity = snapshot.get("capacity")
        if not isinstance(capacity, dict):
            continue
        snapshot_count += 1
        object_id_prefix = snapshot_object_id_prefix(row, snapshot)
        object_id_prefix_counts[object_id_prefix] = object_id_prefix_counts.get(object_id_prefix, 0) + 1
        for key, value in flatten_hicache_capacity_scalars(capacity):
            unique_values.setdefault(key, set()).add(json.dumps(value, ensure_ascii=False, sort_keys=True))
        if len(samples) < 5:
            samples.append(
                {
                    "object_id_prefix": object_id_prefix,
                    "page_size": capacity.get("page_size"),
                    "write_policy": capacity.get("write_policy"),
                    "prefetch_policy": capacity.get("prefetch_policy"),
                    "l1_capacity_pages": capacity.get("l1_capacity_pages"),
                    "l1_available_pages": capacity.get("l1_available_pages"),
                    "l2_capacity_pages": capacity.get("l2_capacity_pages"),
                    "l2_available_pages": capacity.get("l2_available_pages"),
                    "prefetch_threshold_pages": capacity.get("prefetch_threshold_pages"),
                    "prefetch_capacity_limit_pages": capacity.get("prefetch_capacity_limit_pages"),
                }
            )

    return {
        "ready": snapshot_count > 0,
        "snapshot_count": snapshot_count,
        "object_id_prefix_counts": dict(sorted(object_id_prefix_counts.items())),
        "unique_values": {
            key: [json.loads(value) for value in sorted(values)]
            for key, values in sorted(unique_values.items())
        },
        "samples": samples,
    }


def build_hicache_capacity_config_audit(
    capacity_oracle: dict[str, Any],
    target_config: dict[str, Any],
    oracle_final_counts: dict[str, int],
    oracle_observed_max_counts: dict[str, int] | None = None,
) -> dict[str, Any]:
    """检查 C++ target config 与真实 capacity/policy 事实的一致性。

    该结果只做诊断，不直接决定 validation_ready。raw pool capacity 可能大于
    HiCache 对应 tier 的有效可用 budget，因此“低于 observed pool capacity”
    是需要解释的提示，不一定是错误。
    """

    unique_values = capacity_oracle.get("unique_values") if isinstance(capacity_oracle.get("unique_values"), dict) else {}
    observed_max_counts = oracle_observed_max_counts or {}
    target = {
        "page_size": _optional_int(target_config.get("page_size")),
        "l1_capacity_pages": _optional_int(target_config.get("l1_capacity_pages", target_config.get("l1_capacity"))),
        "l2_capacity_pages": _optional_int(target_config.get("l2_capacity_pages", target_config.get("l2_capacity"))),
        "write_policy": _normalize_policy_value(target_config.get("write_policy")),
        "prefetch_policy": _normalize_policy_value(target_config.get("prefetch_policy")),
        "prefetch_threshold_pages": _optional_int(target_config.get("prefetch_threshold_pages")),
        "prefetch_capacity_limit_pages": _optional_int(target_config.get("prefetch_capacity_limit_pages")),
    }
    comparisons = {
        "page_size": _compare_int_config("page_size", target["page_size"], _unique_int_values(unique_values, ["page_size"])),
        "write_policy": _compare_policy_config("write_policy", target["write_policy"], _unique_policy_values(unique_values, ["write_policy"])),
        "prefetch_policy": _compare_policy_config("prefetch_policy", target["prefetch_policy"], _unique_policy_values(unique_values, ["prefetch_policy"])),
        "prefetch_threshold_pages": _compare_int_config(
            "prefetch_threshold_pages",
            target["prefetch_threshold_pages"],
            _unique_int_values(unique_values, ["prefetch_threshold_pages", "thresholds.prefetch_threshold_pages"]),
        ),
        "prefetch_capacity_limit_pages": _compare_int_config(
            "prefetch_capacity_limit_pages",
            target["prefetch_capacity_limit_pages"],
            _unique_int_values(unique_values, ["prefetch_capacity_limit_pages", "thresholds.prefetch_capacity_limit_pages"]),
        ),
        "l1_capacity_pages": _compare_capacity_config(
            "l1_capacity_pages",
            target["l1_capacity_pages"],
            _unique_int_values(unique_values, ["l1_capacity_pages", "l1_pool.capacity_pages"]),
            oracle_final_counts.get("l1_resident_pages"),
            observed_max_counts.get("l1_resident_pages"),
        ),
        "l2_capacity_pages": _compare_capacity_config(
            "l2_capacity_pages",
            target["l2_capacity_pages"],
            _unique_int_values(unique_values, ["l2_capacity_pages", "l2_pool.capacity_pages"]),
            oracle_final_counts.get("l2_resident_pages"),
            observed_max_counts.get("l2_resident_pages"),
        ),
    }
    warnings: list[str] = []
    likely_errors: list[str] = []
    for field, comparison in comparisons.items():
        status = str(comparison.get("status") or "")
        final_status = str(comparison.get("final_count_status") or "")
        max_status = str(comparison.get("observed_max_status") or "")
        if (
            status in {"target_exceeds_observed_pool", "mismatch"}
            or final_status == "target_below_oracle_final_count"
            or max_status == "target_below_oracle_observed_max_count"
        ):
            likely_errors.append(field)
        elif status in {"target_below_observed_pool", "not_configured", "no_observed_value"}:
            warnings.append(field)
    return {
        "ready": bool(capacity_oracle.get("ready")) or bool(target_config),
        "oracle_capacity_ready": bool(capacity_oracle.get("ready")),
        "target_config_ready": bool(target_config),
        "target_config": target,
        "comparisons": comparisons,
        "recommended_target_config": recommend_hicache_target_config(
            unique_values,
            oracle_final_counts,
            observed_max_counts,
            target,
        ),
        "warning_fields": warnings,
        "likely_error_fields": likely_errors,
        "note": "This audit is diagnostic. A target capacity lower than observed pool capacity can be a valid effective budget, but target capacity below oracle final resident count is a likely configuration error.",
    }


def recommend_hicache_target_config(
    capacity_unique_values: dict[str, Any],
    oracle_final_counts: dict[str, int],
    oracle_observed_max_counts: dict[str, int],
    target_config: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """从 target oracle 事实推荐 C++ HiCache target config。

    推荐值只用于修复指导，不在本次 runner 中反向覆盖 C++ 输入。page size
    和 policy 是稳定配置事实，可以自动写入推荐配置；capacity 不从 final
    count 或 observed max 自动反推，因为它们描述的是运行时占用，不等价于
    SGLang 的有效容量。只有调用方已经显式提供 target capacity 时才保留。
    """

    target_config = target_config or {}
    page_size_values = _unique_int_values(capacity_unique_values, ["page_size"])
    write_policy_values = _unique_policy_values(capacity_unique_values, ["write_policy"])
    prefetch_policy_values = _unique_policy_values(capacity_unique_values, ["prefetch_policy"])
    prefetch_threshold_values = _unique_int_values(capacity_unique_values, ["prefetch_threshold_pages", "thresholds.prefetch_threshold_pages"])
    prefetch_capacity_limit_values = _unique_int_values(
        capacity_unique_values,
        ["prefetch_capacity_limit_pages", "thresholds.prefetch_capacity_limit_pages"],
    )
    l1_pool_values = _unique_int_values(capacity_unique_values, ["l1_capacity_pages", "l1_pool.capacity_pages"])
    l2_pool_values = _unique_int_values(capacity_unique_values, ["l2_capacity_pages", "l2_pool.capacity_pages"])

    warnings: list[str] = []
    result: dict[str, Any] = {"enabled": True}
    evidence: dict[str, Any] = {}

    _recommend_single_value(result, evidence, warnings, "page_size", page_size_values)
    _recommend_single_value(result, evidence, warnings, "write_policy", write_policy_values)
    _recommend_single_value(result, evidence, warnings, "prefetch_policy", prefetch_policy_values)
    _recommend_single_value(result, evidence, warnings, "prefetch_threshold_pages", prefetch_threshold_values)
    _recommend_single_value(result, evidence, warnings, "prefetch_capacity_limit_pages", prefetch_capacity_limit_values)
    _recommend_capacity_value(
        result,
        evidence,
        warnings,
        "l1_capacity_pages",
        l1_pool_values,
        oracle_observed_max_counts.get("l1_resident_pages"),
        oracle_final_counts.get("l1_resident_pages"),
        _optional_int(target_config.get("l1_capacity_pages", target_config.get("l1_capacity"))),
    )
    _recommend_capacity_value(
        result,
        evidence,
        warnings,
        "l2_capacity_pages",
        l2_pool_values,
        oracle_observed_max_counts.get("l2_resident_pages"),
        oracle_final_counts.get("l2_resident_pages"),
        _optional_int(target_config.get("l2_capacity_pages", target_config.get("l2_capacity"))),
    )

    required = ("page_size", "write_policy", "prefetch_policy")
    return {
        "ready": all(key in result for key in required),
        "hicache": result,
        "evidence": evidence,
        "warnings": warnings,
        "note": "Recommended config is derived from target oracle facts. Capacity is only copied when it was explicitly configured; observed occupancy peaks are reported as evidence but are not treated as capacity.",
    }


def _recommend_single_value(
    result: dict[str, Any],
    evidence: dict[str, Any],
    warnings: list[str],
    field: str,
    values: list[Any],
) -> None:
    """从唯一观测值生成推荐配置字段；多值或缺失时只写 warning。"""

    if len(values) == 1:
        result[field] = values[0]
        evidence[field] = {"source": "unique_observed_value", "observed_values": values}
    elif not values:
        warnings.append(f"{field}:missing_observed_value")
        evidence[field] = {"source": "missing", "observed_values": []}
    else:
        warnings.append(f"{field}:multiple_observed_values")
        evidence[field] = {"source": "ambiguous", "observed_values": values}


def _recommend_capacity_value(
    result: dict[str, Any],
    evidence: dict[str, Any],
    warnings: list[str],
    field: str,
    pool_values: list[int],
    observed_max_count: int | None,
    final_count: int | None,
    explicit_target_value: int | None = None,
) -> None:
    """推荐容量字段。

    容量只从显式 target config 复制；runtime occupancy 和 pool snapshot 仅作为诊断证据。
    """

    raw_pool = pool_values[0] if len(pool_values) == 1 else None
    if len(pool_values) > 1:
        warnings.append(f"{field}:multiple_observed_pool_values")
    if explicit_target_value is None or explicit_target_value <= 0:
        warnings.append(f"{field}:not_auto_recommended")
        evidence[field] = {
            "source": "not_auto_recommended",
            "observed_pool_values": pool_values,
            "observed_max_count": observed_max_count,
            "final_count": final_count,
            "reason": "Runtime occupancy and pool snapshots are diagnostic evidence, not a stable capacity parameter for target prediction.",
        }
        return
    selected = explicit_target_value
    source = "explicit_target_config"
    if final_count is not None and selected < final_count:
        warnings.append(f"{field}:selected_below_final_count")
    if raw_pool is not None and observed_max_count is not None and observed_max_count > raw_pool:
        warnings.append(f"{field}:observed_max_exceeds_pool")
    result[field] = selected
    evidence[field] = {
        "source": source,
        "observed_pool_values": pool_values,
        "observed_max_count": observed_max_count,
        "final_count": final_count,
        "selected": selected,
    }


def _compare_int_config(field: str, target_value: int | None, observed_values: list[int]) -> dict[str, Any]:
    """比较整数型 target config 和 oracle 观测值。"""

    if target_value is None or target_value <= 0:
        status = "not_configured"
    elif not observed_values:
        status = "no_observed_value"
    elif target_value in observed_values:
        status = "match"
    else:
        status = "mismatch"
    return {
        "field": field,
        "target_value": target_value,
        "observed_values": observed_values,
        "status": status,
    }


def _compare_policy_config(field: str, target_value: str, observed_values: list[str]) -> dict[str, Any]:
    """比较 policy 型 target config 和 oracle 观测值。"""

    if target_value in {"", "observed"}:
        status = "not_configured"
    elif not observed_values:
        status = "no_observed_value"
    elif target_value in observed_values:
        status = "match"
    else:
        status = "mismatch"
    return {
        "field": field,
        "target_value": target_value,
        "observed_values": observed_values,
        "status": status,
    }


def _compare_capacity_config(
    field: str,
    target_value: int | None,
    observed_values: list[int],
    oracle_final_count: int | None,
    oracle_observed_max_count: int | None,
) -> dict[str, Any]:
    """比较容量 config、oracle final count 和 oracle observed max count。"""

    if target_value is None or target_value <= 0:
        status = "not_configured"
    elif not observed_values:
        status = "no_observed_value"
    elif target_value in observed_values:
        status = "matches_observed_pool"
    elif target_value > max(observed_values):
        status = "target_exceeds_observed_pool"
    else:
        status = "target_below_observed_pool"

    if oracle_final_count is None:
        final_status = "no_oracle_final_count"
    elif target_value is None or target_value <= 0:
        final_status = "not_configured"
    elif target_value < oracle_final_count:
        final_status = "target_below_oracle_final_count"
    elif target_value == oracle_final_count:
        final_status = "matches_oracle_final_count"
    else:
        final_status = "target_above_oracle_final_count"

    if oracle_observed_max_count is None:
        observed_max_status = "no_oracle_observed_max_count"
    elif target_value is None or target_value <= 0:
        observed_max_status = "not_configured"
    elif target_value < oracle_observed_max_count:
        observed_max_status = "target_below_oracle_observed_max_count"
    elif target_value == oracle_observed_max_count:
        observed_max_status = "matches_oracle_observed_max_count"
    else:
        observed_max_status = "target_above_oracle_observed_max_count"

    return {
        "field": field,
        "target_value": target_value,
        "observed_pool_values": observed_values,
        "oracle_final_count": oracle_final_count,
        "oracle_observed_max_count": oracle_observed_max_count,
        "status": status,
        "final_count_status": final_status,
        "observed_max_status": observed_max_status,
    }


def _unique_int_values(unique_values: dict[str, Any], keys: list[str]) -> list[int]:
    """从 capacity oracle unique_values 中收集整数值。"""

    values: set[int] = set()
    for key in keys:
        raw_values = unique_values.get(key)
        if not isinstance(raw_values, list):
            continue
        for value in raw_values:
            item = _optional_int(value)
            if item is not None:
                values.add(item)
    return sorted(values)


def _unique_policy_values(unique_values: dict[str, Any], keys: list[str]) -> list[str]:
    """从 capacity oracle unique_values 中收集规整后的 policy 值。"""

    values: set[str] = set()
    for key in keys:
        raw_values = unique_values.get(key)
        if not isinstance(raw_values, list):
            continue
        for value in raw_values:
            normalized = _normalize_policy_value(value)
            if normalized:
                values.add(normalized)
    return sorted(values)


def _optional_int(value: Any) -> int | None:
    """严格解析正整数候选；bool 和非法值返回 None。"""

    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _normalize_policy_value(value: Any) -> str:
    """规整 policy 字符串，使用下划线形式。"""

    if value is None:
        return ""
    return str(value).strip().lower().replace("-", "_")


def flatten_hicache_capacity_scalars(value: Any, prefix: str = "") -> list[tuple[str, Any]]:
    """把 capacity snapshot 中的嵌套标量展开成点分路径。"""

    rows: list[tuple[str, Any]] = []
    if isinstance(value, dict):
        for key, item in sorted(value.items()):
            child_prefix = f"{prefix}.{key}" if prefix else str(key)
            rows.extend(flatten_hicache_capacity_scalars(item, child_prefix))
        return rows
    if value is None or isinstance(value, (list, tuple, set)):
        return rows
    if isinstance(value, (str, int, float, bool)):
        rows.append((prefix, value))
    return rows


def observed_max_derived_state_counts(snapshots: list[dict[str, Any]]) -> dict[str, int]:
    """计算 raw snapshot 时间线上每个状态集合达到过的最大规模。

    final state 只描述 run 结束时的状态，不能代表容量压力峰值。这里按
    HiRadixCache object 时间线更新多进程 state union，再统计峰值，用于
    capacity config audit 判断 target budget 是否低于真实曾经达到过的 resident set。
    """

    timeline: list[tuple[tuple[int, int, int], tuple[str, str, str], dict[str, Any]]] = []
    fallback_states: list[dict[str, Any]] = []
    for row in snapshots:
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
            continue
        if not snapshot_is_hiradix_cache_state(row, snapshot) or not snapshot_is_completed_state(row):
            continue
        state = derived_hicache_state_from_snapshot(snapshot)
        fallback_states.append(state)
        object_id = str(row.get("object_id") or snapshot.get("object_id") or "")
        if not object_id:
            continue
        key = (str(row.get("trace_path") or ""), str(row.get("pid") or ""), object_id)
        timeline.append((snapshot_timeline_sort_key(row), key, row))

    max_counts: dict[str, int] = {}
    if timeline:
        object_states: dict[tuple[str, str, str], dict[str, Any]] = {}
        for _sort_key, key, row in sorted(timeline, key=lambda item: item[0]):
            object_states[key] = derived_hicache_state_from_snapshot(row.get("state_snapshot", {}))
            _update_max_state_counts(max_counts, union_hicache_states(object_states.values()))
        return max_counts

    for state in fallback_states:
        _update_max_state_counts(max_counts, state)
    return max_counts


def _update_max_state_counts(max_counts: dict[str, int], state: dict[str, Any]) -> None:
    """用一个 state 更新每个集合字段达到过的最大规模。"""

    for key, value in state.items():
        if not isinstance(value, list):
            continue
        count = len({str(item) for item in value if item is not None})
        max_counts[str(key)] = max(max_counts.get(str(key), 0), count)


def latest_derived_state(snapshots: list[dict[str, Any]]) -> dict[str, Any]:
    """按进程取最后一个 completed snapshot，并合并成 oracle final state。"""

    latest_by_process: dict[tuple[str, str], dict[str, Any]] = {}
    completed_snapshots = [row for row in snapshots if snapshot_is_completed_state(row)]
    source_snapshots = completed_snapshots if completed_snapshots else snapshots
    for row in source_snapshots:
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
            continue
        if not snapshot_is_hiradix_cache_state(row, snapshot):
            continue
        derived = derived_hicache_state_from_snapshot(snapshot)
        if isinstance(derived, dict) and any(isinstance(derived.get(key), list) for key in derived):
            key = (str(row.get("trace_path") or ""), str(row.get("pid") or ""))
            current = latest_by_process.get(key)
            if current is None or snapshot_sort_key(row) >= snapshot_sort_key(current):
                latest_by_process[key] = row

    union: dict[str, list[str]] = {}
    for row in latest_by_process.values():
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict):
            continue
        if not snapshot_is_hiradix_cache_state(row, snapshot):
            continue
        derived = derived_hicache_state_from_snapshot(snapshot)
        if not isinstance(derived, dict):
            continue
        for key, value in derived.items():
            if not isinstance(value, list):
                continue
            current = union.setdefault(key, [])
            seen = set(current)
            for item in value:
                page = str(item)
                if page not in seen:
                    current.append(page)
                    seen.add(page)
    return {key: sorted(value) for key, value in union.items()}


def derived_hicache_state_from_snapshot(snapshot: dict[str, Any]) -> dict[str, Any]:
    """从 state snapshot 原始节点重新派生集合状态。

    probe 写出的 `derived` 只是调试摘要；真实验证以 `nodes` 中的
    has_device_value/has_host_value/evicted 等原始字段为准，避免摘要和节点
    字段在复杂对象快照中不一致。
    """

    nodes = snapshot.get("nodes")
    if not isinstance(nodes, list):
        derived = snapshot.get("derived")
        return derived if isinstance(derived, dict) else {}

    result: dict[str, set[str]] = {
        "l1_resident_pages": set(),
        "l2_resident_pages": set(),
        "dirty_pages": set(),
        "backuped_pages": set(),
        "evicted_pages": set(),
        "locked_pages": set(),
    }
    for node in nodes:
        if not isinstance(node, dict):
            continue
        pages = page_keys_from_snapshot_hash(node.get("hash_value"))
        has_device_value = bool(node.get("has_device_value"))
        has_host_value = bool(node.get("has_host_value"))
        backuped = bool(node.get("backuped")) or has_host_value
        evicted = bool(node.get("evicted"))
        if has_device_value:
            result["l1_resident_pages"].update(pages)
        if has_host_value:
            result["l2_resident_pages"].update(pages)
        # SGLang HiRadixCache 目前没有可靠暴露 dirty 字段；write-back 下 device-resident 且未备份到 host 的页就是 state model 需要维护的 dirty 页。
        if node.get("dirty") or (has_device_value and not backuped and not evicted):
            result["dirty_pages"].update(pages)
        if backuped:
            result["backuped_pages"].update(pages)
        if evicted:
            result["evicted_pages"].update(pages)
        if int(optional_float(node.get("lock_ref")) or 0) > 0 or int(optional_float(node.get("host_ref_counter")) or 0) > 0:
            result["locked_pages"].update(pages)
    return {key: sorted(value) for key, value in result.items()}


def page_keys_from_snapshot_hash(value: Any) -> list[str]:
    """从 snapshot hash_value 字段提取 page key 列表。"""

    if value is None:
        return []
    if isinstance(value, list):
        return [str(item) for item in value if item is not None]
    return [str(value)]


def normalize_hicache_state_for_oracle_compare(state: dict[str, Any], page_key_mode: str) -> dict[str, Any]:
    """按 page_key_mode 归一化 state 中的集合字段。"""

    normalized: dict[str, Any] = {}
    for key, value in state.items():
        if isinstance(value, list):
            normalized[key] = sorted({normalize_hicache_page_key(item, page_key_mode) for item in value if item is not None})
        else:
            normalized[key] = value
    return normalized


def normalize_hicache_page_key(value: Any, page_key_mode: str) -> str:
    """归一化单个 page key；strip_scope 模式会去掉 scope 前缀。"""

    page = str(value)
    if page_key_mode == "strip_scope" and "|" in page:
        return page.split("|", 1)[1]
    return page


def snapshot_sort_key(row: dict[str, Any]) -> tuple[int, int, int]:
    """state snapshot 的逻辑顺序。

    Python probe 的 start/end 事件在 merged trace 中可能同 timestamp 且顺序反转。
    同一时刻优先使用 end 快照，它更接近一次调用完成后的真实状态。
    """

    ts = int(optional_float(row.get("ts")) or 0)
    source_name = str(row.get("source_event_name") or row.get("event_name") or "")
    phase_score = 1 if source_name.endswith("_end") else 0
    order = int(row.get("order") or 0)
    return (ts, phase_score, order)


def diff_hicache_sets(model_final: dict[str, Any], oracle_final: dict[str, Any]) -> dict[str, Any]:
    """比较 model final state 和 oracle final state 的集合字段。"""

    keys = [
        "l1_resident_pages",
        "l2_resident_pages",
        "l3_resident_pages",
        "dirty_pages",
        "backuped_pages",
        "evicted_pages",
        "locked_pages",
        "prefetch_planned_pages",
        "prefetch_ready_pages",
        "prefetch_late_pages",
        "prefetch_suppressed_pages",
    ]
    diff: dict[str, Any] = {}
    for key in keys:
        if key not in oracle_final:
            continue
        model_set = set(str(item) for item in model_final.get(key, []) if item is not None)
        oracle_set = set(str(item) for item in oracle_final.get(key, []) if item is not None)
        missing = sorted(oracle_set - model_set)
        extra = sorted(model_set - oracle_set)
        diff[key] = {
            "match": not missing and not extra,
            "missing_in_model": missing,
            "extra_in_model": extra,
            "model_count": len(model_set),
            "oracle_count": len(oracle_set),
        }
    return diff


def final_state_counts(state: dict[str, Any]) -> dict[str, int]:
    """统计 final state 中所有集合字段的大小，帮助暴露未参与 diff 的状态。"""

    counts: dict[str, int] = {}
    for key, value in sorted(state.items()):
        if isinstance(value, list):
            counts[key] = len({str(item) for item in value if item is not None})
    return counts


def unchecked_model_state_keys(model_final: dict[str, Any], oracle_final: dict[str, Any]) -> list[str]:
    """列出 model 有、但 oracle snapshot 没有的集合字段。

    这些字段不会参与 `final_state_match`，但必须在文档和后续 probe 设计中显式处理。
    """

    keys: list[str] = []
    for key, value in sorted(model_final.items()):
        if isinstance(value, list) and key not in oracle_final and any(item is not None for item in value):
            keys.append(key)
    return keys


def load_predicted_state_records(path: Path | None) -> list[dict[str, Any]]:
    """读取 C++ HiCache state model 输出的 transition 明细。

    缺失或损坏的文件只代表该次验证没有 transition 明细，不应让上层
    profiling/modeling 编排因为诊断附件不可用而失败。
    """

    if path is None or not path.is_file():
        return []
    try:
        payload = load_json(path)
    except json.JSONDecodeError:
        return []
    records = payload.get("records") if isinstance(payload, dict) else []
    return [record for record in records if isinstance(record, dict)] if isinstance(records, list) else []


def build_request_transition_coverage(predicted_records: list[dict[str, Any]], snapshots: list[dict[str, Any]]) -> dict[str, Any]:
    """生成 request 级 state trace 覆盖摘要。

    真实 state snapshot 目前不是严格的一次 transition oracle；它更像调用点快照。
    因此这里先做 request id 覆盖检查，帮助后续把 final set mismatch 下钻到请求。
    没有 request_id 的旧 trace 不参与覆盖判定。
    """

    predicted_requests = sorted({str(row.get("request_id")) for row in predicted_records if str(row.get("request_id") or "")})
    oracle_requests = sorted({str(row.get("request_id")) for row in snapshots if str(row.get("request_id") or "")})
    predicted_set = set(predicted_requests)
    oracle_set = set(oracle_requests)
    return {
        "ready": bool(predicted_requests or oracle_requests),
        "predicted_request_count": len(predicted_requests),
        "oracle_request_count": len(oracle_requests),
        "requests_missing_oracle_snapshot": sorted(predicted_set - oracle_set),
        "requests_missing_predicted_transition": sorted(oracle_set - predicted_set),
        "predicted_transition_count_by_request": count_records_by_key(predicted_records, "request_id"),
        "oracle_snapshot_count_by_request": count_records_by_key(snapshots, "request_id"),
    }


def build_transition_coverage(predicted_records: list[dict[str, Any]], snapshots: list[dict[str, Any]]) -> dict[str, Any]:
    """生成 transition 级覆盖摘要。

    这里不把 coverage 作为验证通过条件。state snapshot 是稀疏观测，不是逐步
    transition oracle；该摘要用于定位“哪些 transition / page / request 缺少解释”。
    """

    predicted_pages = sorted(
        {
            str(page)
            for record in predicted_records
            for page in page_set_from_predicted_record(record)
            if page is not None
        }
    )
    oracle_pages = sorted(
        {
            str(page)
            for snapshot in snapshots
            for pages in derived_hicache_state_from_snapshot(snapshot.get("state_snapshot", {})).values()
            if isinstance(pages, list)
            for page in pages
            if page is not None
        }
    )
    predicted_page_set = set(predicted_pages)
    oracle_page_set = set(oracle_pages)
    return {
        "ready": bool(predicted_records),
        "predicted_transition_count": len(predicted_records),
        "predicted_transition_count_by_kind": count_records_by_key(predicted_records, "transition_kind"),
        "predicted_operation_count_by_kind": count_records_by_key(predicted_records, "predicted_operation_kind"),
        "predicted_transition_count_by_source_event": count_records_by_key(predicted_records, "source_event_name"),
        "oracle_snapshot_count_by_target": count_records_by_key(snapshots, "target_id"),
        "predicted_page_count": len(predicted_pages),
        "oracle_page_count": len(oracle_pages),
        "pages_missing_predicted_transition": sorted(oracle_page_set - predicted_page_set),
        "pages_without_oracle_snapshot_evidence": sorted(predicted_page_set - oracle_page_set),
    }


DELTA_KIND_BY_STATE_KEY: dict[str, tuple[str, str]] = {
    "l1_resident_pages": ("add_l1_resident", "remove_l1_resident"),
    "l2_resident_pages": ("add_l2_resident", "remove_l2_resident"),
    "dirty_pages": ("mark_dirty", "clear_dirty"),
    "backuped_pages": ("mark_backuped", "clear_backuped"),
    "evicted_pages": ("mark_evicted", "clear_evicted"),
    "locked_pages": ("mark_locked", "clear_locked"),
}


def build_event_delta_validation(predicted_records: list[dict[str, Any]], snapshots: list[dict[str, Any]]) -> dict[str, Any]:
    """把 state snapshot 的 start/end 差分变成事件级 oracle。

    该校验只对同一条 trace 的 self-config prediction 有严格意义。跨配置 prediction 的 base trace 和 target
    oracle trace 时间戳不同，无法用事件 key 精确对齐；这种情况下仍输出 oracle/predicted 摘要，
    但不作为 validation_ready 的硬门槛。

    Python probe 的 state snapshot 是调用包围式采样：外层函数的 end snapshot 会包含内层
    HiCache 调用造成的状态变化。为了避免把“归因不同”误判成“状态推导错误”，这里同时输出：

    - inclusive oracle：保留所有包围差分，只用于观察真实调用包含了哪些状态变化；
    - exclusive oracle：只比较没有嵌套 state snapshot 的调用，用于 self-config prediction 的严格事件级
      检查候选。
    """

    active_state_keys = active_delta_state_keys(predicted_records)
    oracle = build_oracle_event_deltas(snapshots, active_state_keys)
    predicted = build_predicted_event_deltas(predicted_records)
    predicted_event_keys = {str(row.get("event_key") or "") for row in predicted["rows"]}
    oracle_event_keys = {str(row.get("event_key") or "") for row in oracle["exclusive_rows"]}
    shared_event_keys = sorted(predicted_event_keys & oracle_event_keys)
    comparable = bool(shared_event_keys)
    mismatches = compare_event_delta_rows(predicted["rows"], oracle["exclusive_rows"], set(shared_event_keys)) if comparable else []
    return {
        "ready": bool(oracle["inclusive_rows"]),
        "comparable": comparable,
        "match": comparable and not mismatches,
        "oracle_paired_event_count": oracle["paired_event_count"],
        "oracle_transition_count": len(oracle["exclusive_rows"]),
        "inclusive_oracle_transition_count": len(oracle["inclusive_rows"]),
        "predicted_comparable_transition_count": len(predicted["rows"]),
        "shared_event_key_count": len(shared_event_keys),
        "exclusive_oracle_event_key_count": len(oracle_event_keys),
        "predicted_event_key_count": len(predicted_event_keys),
        "exclusive_oracle_event_keys_missing_predicted": sorted(oracle_event_keys - predicted_event_keys)[:50],
        "predicted_event_keys_without_exclusive_oracle": sorted(predicted_event_keys - oracle_event_keys)[:50],
        "oracle_transition_count_by_kind": count_rows_by_transition_kind(oracle["exclusive_rows"]),
        "inclusive_oracle_transition_count_by_kind": count_rows_by_transition_kind(oracle["inclusive_rows"]),
        "predicted_comparable_transition_count_by_kind": count_rows_by_transition_kind(predicted["rows"]),
        "unpaired_snapshot_count": oracle["unpaired_snapshot_count"],
        "nested_event_count": oracle["nested_event_count"],
        "nested_oracle_transition_count": oracle["nested_transition_count"],
        "ignored_state_keys_without_predicted_transition": oracle["ignored_state_keys"],
        "mismatch_count": len(mismatches),
        "mismatch_totals_by_kind": summarize_delta_mismatches_by_kind(mismatches),
        "top_mismatches": mismatches[:20],
        "note": "Exact event delta comparison is intended for self-config prediction; cross-config prediction should use final-state and policy oracle fields.",
    }


def build_timeline_delta_validation(predicted_records: list[dict[str, Any]], snapshots: list[dict[str, Any]]) -> dict[str, Any]:
    """按 cache object 的 snapshot 时间线生成一次性状态变化 oracle。

    `event_delta_validation` 比较 start/end 包围差分，适合定位某个调用点；
    timeline oracle 则沿同一个 cache object 的状态快照前后推进，每次只比较相邻
    snapshot 的净变化，避免外层/内层包围差分重复计数。

    该验证依赖 profiling snapshot 提供 `object_id`。旧 trace 没有对象身份时只输出
    `ready=false`，不影响既有 validation gate。
    """

    active_state_keys = active_delta_state_keys(predicted_records)
    visible_state_keys = timeline_visible_state_keys(snapshots)
    comparison_state_keys = active_state_keys & visible_state_keys
    oracle = build_oracle_timeline_deltas(snapshots, comparison_state_keys)
    oracle_rows = list(oracle["rows"])
    predicted = build_predicted_event_deltas(predicted_records, comparison_state_keys)
    comparable = bool(oracle["rows"])
    mismatches = compare_delta_multisets(predicted["rows"], oracle_rows) if comparable else []
    model_extra_transition_count = sum(int(row.get("extra_in_predicted", 0) or 0) for row in mismatches)
    oracle_extra_transition_count = sum(int(row.get("missing_in_predicted", 0) or 0) for row in mismatches)
    model_transition_covered = comparable and model_extra_transition_count == 0
    predicted_counts = count_rows_by_transition_kind(predicted["rows"])
    oracle_counts = count_rows_by_transition_kind(oracle_rows)
    return {
        "ready": comparable,
        "match": model_transition_covered,
        "exact_match": comparable and not mismatches,
        "model_transition_covered": model_transition_covered,
        "model_extra_transition_count": model_extra_transition_count,
        "oracle_extra_transition_count": oracle_extra_transition_count,
        "oracle_transition_count": len(oracle_rows),
        "predicted_transition_count": len(predicted["rows"]),
        "compared_state_keys": sorted(comparison_state_keys),
        "ignored_unobservable_state_keys": sorted(active_state_keys - comparison_state_keys),
        "oracle_transition_count_by_kind": oracle_counts,
        "predicted_transition_count_by_kind": predicted_counts,
        "raw_mismatch_count": len(mismatches),
        "raw_top_mismatches": mismatches[:20],
        "object_group_count": oracle["object_group_count"],
        "snapshot_count_with_object_id": oracle["snapshot_count_with_object_id"],
        "snapshot_count_without_object_id": oracle["snapshot_count_without_object_id"],
        "ignored_snapshot_count": oracle["ignored_snapshot_count"],
        "ignored_state_keys_without_predicted_transition": oracle["ignored_state_keys"],
        "mismatch_count": len(mismatches),
        "mismatch_totals_by_kind": summarize_delta_mismatches_by_kind(mismatches),
        "top_mismatches": mismatches[:20],
        "note": "Timeline delta comparison requires state_snapshot.object_id and compares transition kind/page multisets. match=true means every predicted transition is covered by the raw snapshot timeline; exact_match=false can still occur when sparse multi-process snapshots expose oracle-only transient state oscillations.",
    }


def timeline_visible_state_keys(snapshots: list[dict[str, Any]]) -> set[str]:
    """返回 raw snapshot timeline 实际暴露过的 state key。"""

    visible: set[str] = set()
    for row in snapshots:
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
            continue
        if not snapshot_is_hiradix_cache_state(row, snapshot) or not snapshot_is_completed_state(row):
            continue
        state = derived_hicache_state_from_snapshot(snapshot)
        for key, value in state.items():
            if isinstance(value, list) and value:
                visible.add(str(key))
    return visible


def build_oracle_timeline_deltas(snapshots: list[dict[str, Any]], active_state_keys: set[str]) -> dict[str, Any]:
    """沿 raw state snapshot 时间线构造 oracle transition multiset。

    timeline oracle 先按 cache object 汇总，再对所有对象状态取 union 后求相邻差分。
    这与 C++ state model 的最终可见页集合语义一致，避免多对象/多进程 snapshot
    让同一页被错误地来回清空。
    """

    timeline: list[tuple[tuple[int, int, int], tuple[str, str, str], dict[str, Any]]] = []
    snapshot_count_with_object_id = 0
    snapshot_count_without_object_id = 0
    ignored_snapshot_count = 0
    for row in snapshots:
        snapshot = row.get("state_snapshot")
        if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
            ignored_snapshot_count += 1
            continue
        # controller snapshot 只描述队列，不是 HiRadixCache tree state。timeline oracle 只比较
        # cache tree object，避免空 controller snapshot 把 page 集合反复清空。
        if not snapshot_is_hiradix_cache_state(row, snapshot):
            ignored_snapshot_count += 1
            continue
        object_id = str(row.get("object_id") or snapshot.get("object_id") or "")
        if not object_id:
            snapshot_count_without_object_id += 1
            continue
        if not snapshot_is_completed_state(row):
            ignored_snapshot_count += 1
            continue
        snapshot_count_with_object_id += 1
        key = (str(row.get("trace_path") or ""), str(row.get("pid") or ""), object_id)
        timeline.append((snapshot_timeline_sort_key(row), key, row))

    rows: list[dict[str, Any]] = []
    ignored_state_keys: set[str] = set()
    object_states: dict[tuple[str, str, str], dict[str, Any]] = {}
    previous_union: dict[str, Any] | None = {}
    object_groups_seen: set[tuple[str, str, str]] = set()
    for _sort_key, key, row in sorted(timeline, key=lambda item: item[0]):
        object_groups_seen.add(key)
        object_states[key] = derived_hicache_state_from_snapshot(row.get("state_snapshot", {}))
        current_union = union_hicache_states(object_states.values())
        if previous_union is not None:
            trace_path, pid, object_id = key
            delta_key = (
                trace_path,
                pid,
                str(row.get("tid") or ""),
                str(row.get("target_id") or ""),
                str(row.get("request_id") or ""),
                str(row.get("operation_id") or ""),
                snapshot_logical_time_us(row),
                event_base_name(str(row.get("source_event_name") or row.get("event_name") or "")),
            )
            delta_result = delta_rows_for_event_key(delta_key, previous_union, current_union, active_state_keys)
            for item in delta_result["rows"]:
                item["object_id"] = object_id
                item["source_event_name"] = str(row.get("source_event_name") or row.get("event_name") or "")
                rows.append(item)
            ignored_state_keys.update(delta_result["ignored_state_keys"])
        previous_union = current_union

    return {
        "rows": rows,
        "final_state": previous_union or {},
        "object_group_count": len(object_groups_seen),
        "snapshot_count_with_object_id": snapshot_count_with_object_id,
        "snapshot_count_without_object_id": snapshot_count_without_object_id,
        "ignored_snapshot_count": ignored_snapshot_count,
        "ignored_state_keys": sorted(ignored_state_keys),
    }

def union_hicache_states(states: Any) -> dict[str, list[str]]:
    """把多个 cache object 的集合状态合并成一次全局可见状态。"""

    union: dict[str, set[str]] = {}
    for state in states:
        if not isinstance(state, dict):
            continue
        for key, value in state.items():
            if not isinstance(value, list):
                continue
            target = union.setdefault(str(key), set())
            target.update(str(item) for item in value if item is not None)
    return {key: sorted(value) for key, value in union.items()}


def snapshot_object_id_prefix(row: dict[str, Any], snapshot: dict[str, Any]) -> str:
    """Return the class-like prefix embedded in snapshot object_id."""

    object_id = str(row.get("object_id") or snapshot.get("object_id") or "")
    return object_id.split(":", 1)[0] if object_id else "unknown"


def snapshot_is_hiradix_cache_state(row: dict[str, Any], snapshot: dict[str, Any]) -> bool:
    """判断 snapshot 是否来自 HiCache state model 要验证的 HiRadixCache 对象。"""

    return snapshot_object_id_prefix(row, snapshot) == "HiRadixCache"


def snapshot_is_completed_state(row: dict[str, Any]) -> bool:
    """判断 state snapshot 是否代表一次调用完成后的状态。

    Python probe 会同时输出 start/end 包围快照。start 快照描述调用前状态，
    如果把它当作最终状态，trace 尾部缺少对应 end snapshot 时会把尚未释放的
    lock/ref 误判为最终 cache state。因此 final oracle 和 timeline oracle 只
    使用 end 或无 phase 的快照；完全没有 completed snapshot 时 final oracle
    才由调用方 fallback 到原始快照集合。
    """

    source_name = str(row.get("source_event_name") or row.get("event_name") or "")
    return event_phase(source_name) != "start"


def snapshot_logical_time_us(row: dict[str, Any]) -> int:
    """返回 snapshot 参与 timeline 排序的逻辑时间。

    end snapshot 的真实状态变化点在 duration 末尾，因此使用 `ts + dur`。
    """

    ts = int(optional_float(row.get("ts")) or 0)
    source_name = str(row.get("source_event_name") or row.get("event_name") or "")
    if event_phase(source_name) == "end":
        ts += int(optional_float(row.get("dur")) or 0)
    return ts


def snapshot_timeline_sort_key(row: dict[str, Any]) -> tuple[int, int, int]:
    """生成稳定 timeline 排序键，确保同时间戳下 start 先于 end。"""

    source_name = str(row.get("source_event_name") or row.get("event_name") or "")
    phase_score = 0 if event_phase(source_name) == "start" else 1
    return (snapshot_logical_time_us(row), phase_score, int(row.get("order") or 0))


def active_delta_state_keys(predicted_records: list[dict[str, Any]]) -> set[str]:
    """根据模型实际输出决定事件级 oracle 需要比较哪些状态集合。

    旧 profiling run 可能能从 snapshot 看到 locked pages，但没有采集 inc/dec lock facts。此时
    C++ 模型无法为 lock 生成 transition，事件级 oracle 应把 lock 标成未比较字段，而不是把它
    混入 state prediction mismatch。
    """

    active_kinds = {str(record.get("transition_kind") or "") for record in predicted_records if isinstance(record, dict)}
    active: set[str] = set()
    for state_key, kinds in DELTA_KIND_BY_STATE_KEY.items():
        if any(kind in active_kinds for kind in kinds):
            active.add(state_key)
    return active


def build_oracle_event_deltas(snapshots: list[dict[str, Any]], active_state_keys: set[str]) -> dict[str, Any]:
    """把 start/end 成对 snapshot 转成事件包围区间差分。

    该函数同时输出 inclusive 和 exclusive 两套结果：inclusive 保留嵌套调用造成
    的全部净变化；exclusive 排除包含其他 snapshot 的外层调用，用于更严格的
    self-config prediction 对齐。
    """

    groups: dict[tuple[str, str, str, str, str, str, int, str], dict[str, dict[str, Any]]] = {}
    for row in snapshots:
        source_name = str(row.get("source_event_name") or row.get("event_name") or "")
        phase = event_phase(source_name)
        if phase not in {"start", "end"}:
            continue
        key = snapshot_event_key(row, source_name)
        group = groups.setdefault(key, {})
        current = group.get(phase)
        if current is None or snapshot_sort_key(row) >= snapshot_sort_key(current):
            group[phase] = row

    inclusive_rows: list[dict[str, Any]] = []
    exclusive_rows: list[dict[str, Any]] = []
    paired = 0
    unpaired = 0
    nested_event_count = 0
    nested_transition_count = 0
    ignored_state_keys: set[str] = set()
    paired_groups: list[tuple[tuple[str, str, str, str, str, str, int, str], dict[str, dict[str, Any]]]] = [
        (key, group) for key, group in sorted(groups.items(), key=lambda item: item[0]) if group.get("start") is not None and group.get("end") is not None
    ]
    for key, group in sorted(groups.items(), key=lambda item: item[0]):
        start = group.get("start")
        end = group.get("end")
        if start is None or end is None:
            unpaired += 1
            continue
        paired += 1
        start_state = derived_hicache_state_from_snapshot(start.get("state_snapshot", {}))
        end_state = derived_hicache_state_from_snapshot(end.get("state_snapshot", {}))
        delta_result = delta_rows_for_event_key(key, start_state, end_state, active_state_keys)
        rows = delta_result["rows"]
        inclusive_rows.extend(rows)
        ignored_state_keys.update(delta_result["ignored_state_keys"])
        if event_has_nested_snapshots(key, group, paired_groups):
            if rows:
                nested_event_count += 1
                nested_transition_count += len(rows)
            continue
        exclusive_rows.extend(rows)
    return {
        "inclusive_rows": inclusive_rows,
        "exclusive_rows": exclusive_rows,
        "paired_event_count": paired,
        "unpaired_snapshot_count": unpaired,
        "nested_event_count": nested_event_count,
        "nested_transition_count": nested_transition_count,
        "ignored_state_keys": sorted(ignored_state_keys),
    }


def event_has_nested_snapshots(
    key: tuple[str, str, str, str, str, str, int, str],
    group: dict[str, dict[str, Any]],
    paired_groups: list[tuple[tuple[str, str, str, str, str, str, int, str], dict[str, dict[str, Any]]]],
) -> bool:
    """判断某个 start/end snapshot 包围区间内是否存在其他 HiCache snapshot。

    这里只按同 trace/pid/tid 判断嵌套关系。不同线程或不同进程的 snapshot 不应影响当前事件的
    exclusive oracle。
    """

    start = group.get("start")
    end = group.get("end")
    if start is None or end is None:
        return False
    start_ts = int(optional_float(start.get("ts")) or 0)
    end_ts = int(optional_float(end.get("ts")) or start_ts)
    end_dur = int(optional_float(end.get("dur")) or 0)
    start_order = int(start.get("order") or 0)
    end_order = int(end.get("order") or start_order)

    interval_start = min(start_ts, end_ts)
    interval_end = max(start_ts, end_ts + end_dur)
    order_start = min(start_order, end_order)
    order_end = max(start_order, end_order)
    if interval_end <= interval_start and order_end <= order_start:
        return False
    trace_path, pid, tid, *_ = key
    for other_key, other_group in paired_groups:
        if other_key == key:
            continue
        other_trace_path, other_pid, other_tid, *_ = other_key
        if (other_trace_path, other_pid, other_tid) != (trace_path, pid, tid):
            continue
        other_start = other_group.get("start")
        other_end = other_group.get("end")
        if other_start is None or other_end is None:
            continue
        other_start_ts = int(optional_float(other_start.get("ts")) or 0)
        other_end_ts = int(optional_float(other_end.get("ts")) or other_start_ts)
        other_start_order = int(other_start.get("order") or 0)
        other_end_order = int(other_end.get("order") or other_start_order)
        if interval_end > interval_start:
            other_interval_start = min(other_start_ts, other_end_ts)
            if interval_start < other_interval_start < interval_end:
                return True
        if order_end > order_start and (order_start < other_start_order < order_end or order_start < other_end_order < order_end):
            return True
    return False


def snapshot_event_key(row: dict[str, Any], source_name: str) -> tuple[str, str, str, str, str, str, int, str]:
    """生成 start/end snapshot 配对使用的事件身份。"""

    return (
        str(row.get("trace_path") or ""),
        str(row.get("pid") or ""),
        str(row.get("tid") or ""),
        str(row.get("target_id") or ""),
        str(row.get("request_id") or ""),
        str(row.get("operation_id") or ""),
        int(optional_float(row.get("ts")) or 0),
        event_base_name(source_name),
    )


def event_phase(name: str) -> str:
    """从 probe event name 后缀解析 start/end phase。"""

    clean = name.split(":", 1)[0]
    if clean.endswith("_start"):
        return "start"
    if clean.endswith("_end"):
        return "end"
    return ""


def event_base_name(name: str) -> str:
    """去掉 start/end 后缀，得到 prediction 和 oracle 共享的事件基名。"""

    clean = name.split(":", 1)[0]
    if clean.endswith("_start"):
        return clean[: -len("_start")]
    if clean.endswith("_end"):
        return clean[: -len("_end")]
    return clean


def delta_rows_for_event_key(
    key: tuple[str, str, str, str, str, str, int, str],
    start_state: dict[str, Any],
    end_state: dict[str, Any],
    active_state_keys: set[str],
) -> dict[str, Any]:
    """把两个集合状态之间的差分展开成 transition rows。

    只有 C++ 模型实际输出过的 state key 会参与比较；其他 snapshot 中可见但
    没有输入事实支撑的变化会被记录到 ignored_state_keys。
    """

    trace_path, pid, tid, target_id, request_id, operation_id, ts, base_name = key
    rows: list[dict[str, Any]] = []
    ignored_state_keys: set[str] = set()
    for state_key, (add_kind, remove_kind) in DELTA_KIND_BY_STATE_KEY.items():
        start_set = set(str(item) for item in start_state.get(state_key, []) if item is not None)
        end_set = set(str(item) for item in end_state.get(state_key, []) if item is not None)
        if state_key not in active_state_keys:
            if start_set != end_set:
                ignored_state_keys.add(state_key)
            continue
        for kind, pages in ((add_kind, sorted(end_set - start_set)), (remove_kind, sorted(start_set - end_set))):
            if not pages:
                continue
            rows.append(
                {
                    "event_key": event_delta_key(pid, ts, base_name),
                    "trace_path": trace_path,
                    "cache_scope": pid,
                    "tid": tid,
                    "target_id": target_id,
                    "request_id": request_id,
                    "operation_id": operation_id,
                    "ts": ts,
                    "event_base_name": base_name,
                    "transition_kind": kind,
                    "pages": pages,
                }
            )
    return {"rows": rows, "ignored_state_keys": sorted(ignored_state_keys)}


def build_predicted_event_deltas(predicted_records: list[dict[str, Any]], active_state_keys: set[str] | None = None) -> dict[str, Any]:
    """把 C++ transition 明细规整成事件级可比较 delta rows。"""

    state_keys = active_state_keys if active_state_keys is not None else set(DELTA_KIND_BY_STATE_KEY)
    comparable_kinds = {kind for state_key, kinds in DELTA_KIND_BY_STATE_KEY.items() if state_key in state_keys for kind in kinds}
    grouped: dict[tuple[str, str], set[str]] = {}
    metadata: dict[tuple[str, str], dict[str, Any]] = {}
    for record in predicted_records:
        kind = str(record.get("transition_kind") or "")
        if kind not in comparable_kinds:
            continue
        pages = [str(page) for page in page_set_from_predicted_record(record) if page is not None]
        if not pages:
            continue
        cache_scope = str(record.get("cache_scope") or "")
        ts = int(optional_float(record.get("ts")) or 0)
        base_name = str(record.get("event_base_name") or event_base_name(str(record.get("source_event_name") or "")))
        key = (event_delta_key(cache_scope, ts, base_name), kind)
        grouped.setdefault(key, set()).update(pages)
        metadata.setdefault(
            key,
            {
                "event_key": key[0],
                "cache_scope": cache_scope,
                "ts": ts,
                "event_base_name": base_name,
                "transition_kind": kind,
            },
        )
    rows = []
    for key, pages in sorted(grouped.items()):
        row = dict(metadata[key])
        row["pages"] = sorted(pages)
        rows.append(row)
    return {"rows": rows}


def event_delta_key(cache_scope: str, ts: int, base_name: str) -> str:
    """生成 prediction/oracle 事件级 delta 的紧凑匹配键。"""

    return f"{cache_scope}:{ts}:{base_name}"


def compare_event_delta_rows(
    predicted_rows: list[dict[str, Any]],
    oracle_rows: list[dict[str, Any]],
    allowed_event_keys: set[str] | None = None,
) -> list[dict[str, Any]]:
    """按事件键和 transition kind 比较 predicted/oracle 页集合。"""

    predicted_by_key = {(str(row.get("event_key") or ""), str(row.get("transition_kind") or "")): row for row in predicted_rows}
    oracle_by_key = {(str(row.get("event_key") or ""), str(row.get("transition_kind") or "")): row for row in oracle_rows}
    mismatches: list[dict[str, Any]] = []
    for key in sorted(set(predicted_by_key) | set(oracle_by_key)):
        if allowed_event_keys is not None and key[0] not in allowed_event_keys:
            continue
        predicted = predicted_by_key.get(key)
        oracle = oracle_by_key.get(key)
        predicted_pages = set(str(page) for page in (predicted or {}).get("pages", []) if page is not None)
        oracle_pages = set(str(page) for page in (oracle or {}).get("pages", []) if page is not None)
        missing = sorted(oracle_pages - predicted_pages)
        extra = sorted(predicted_pages - oracle_pages)
        if missing or extra:
            event_key, kind = key
            mismatches.append(
                {
                    "event_key": event_key,
                    "transition_kind": kind,
                    "missing_in_predicted": missing,
                    "extra_in_predicted": extra,
                    "predicted_count": len(predicted_pages),
                    "oracle_count": len(oracle_pages),
                }
            )
    return mismatches


def compare_delta_multisets(predicted_rows: list[dict[str, Any]], oracle_rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """按 `(transition_kind, page)` multiset 比较 timeline delta。"""

    predicted_counts = delta_multiset_counts(predicted_rows)
    oracle_counts = delta_multiset_counts(oracle_rows)
    mismatches: list[dict[str, Any]] = []
    for key in sorted(set(predicted_counts) | set(oracle_counts)):
        predicted_count = predicted_counts.get(key, 0)
        oracle_count = oracle_counts.get(key, 0)
        if predicted_count == oracle_count:
            continue
        transition_kind, page = key
        mismatches.append(
            {
                "transition_kind": transition_kind,
                "page": page,
                "predicted_count": predicted_count,
                "oracle_count": oracle_count,
                "missing_in_predicted": max(oracle_count - predicted_count, 0),
                "extra_in_predicted": max(predicted_count - oracle_count, 0),
            }
        )
    return mismatches


def mismatch_value_count(value: Any) -> int:
    """把 mismatch 字段里的 list/count 统一折算成数量。"""

    if isinstance(value, list):
        return len([item for item in value if item is not None])
    try:
        return int(value or 0)
    except (TypeError, ValueError):
        return 0


def summarize_delta_mismatches_by_kind(mismatches: list[dict[str, Any]]) -> dict[str, dict[str, int]]:
    """按 transition kind 汇总 delta mismatch，避免只看 top rows 时漏掉主因。"""

    summary: dict[str, dict[str, int]] = {}
    for row in mismatches:
        kind = str(row.get("transition_kind") or "")
        if not kind:
            continue
        item = summary.setdefault(
            kind,
            {
                "mismatch_rows": 0,
                "missing_in_predicted": 0,
                "extra_in_predicted": 0,
                "predicted_count": 0,
                "oracle_count": 0,
            },
        )
        item["mismatch_rows"] += 1
        item["missing_in_predicted"] += mismatch_value_count(row.get("missing_in_predicted"))
        item["extra_in_predicted"] += mismatch_value_count(row.get("extra_in_predicted"))
        item["predicted_count"] += mismatch_value_count(row.get("predicted_count"))
        item["oracle_count"] += mismatch_value_count(row.get("oracle_count"))
    return {kind: summary[kind] for kind in sorted(summary)}


def delta_multiset_counts(rows: list[dict[str, Any]]) -> dict[tuple[str, str], int]:
    """统计 delta rows 中每个 transition/page 出现次数。"""

    counts: dict[tuple[str, str], int] = {}
    for row in rows:
        kind = str(row.get("transition_kind") or "")
        if not kind:
            continue
        for page in row.get("pages", []):
            if page is None:
                continue
            key = (kind, str(page))
            counts[key] = counts.get(key, 0) + 1
    return counts


def count_rows_by_transition_kind(rows: list[dict[str, Any]]) -> dict[str, int]:
    """按 transition kind 汇总触达页数。"""

    counts: dict[str, int] = {}
    for row in rows:
        kind = str(row.get("transition_kind") or "")
        if not kind:
            continue
        counts[kind] = counts.get(kind, 0) + len([page for page in row.get("pages", []) if page is not None])
    return dict(sorted(counts.items()))


def page_set_from_predicted_record(record: dict[str, Any]) -> list[Any]:
    """读取 C++ transition 明细中的目标页集合。"""

    pages = record.get("target_page_set")
    return pages if isinstance(pages, list) else []


def count_records_by_key(rows: list[dict[str, Any]], key: str) -> dict[str, int]:
    """按指定字段统计记录数量，忽略空字段值。"""

    counts: dict[str, int] = {}
    for row in rows:
        value = str(row.get(key) or "")
        if not value:
            continue
        counts[value] = counts.get(value, 0) + 1
    return dict(sorted(counts.items()))


def first_hicache_mismatch(sets_diff: dict[str, Any], predicted_records: list[dict[str, Any]] | None = None) -> dict[str, Any] | None:
    """返回第一个 final-state mismatch 及其候选 transition 证据。"""

    predicted_records = predicted_records or []
    for key, value in sets_diff.items():
        if isinstance(value, dict) and not value.get("match", False):
            missing = value.get("missing_in_model", [])
            extra = value.get("extra_in_model", [])
            page = str((missing or extra or [""])[0])
            return {
                "tier": key,
                "page": page,
                "missing_in_model": missing,
                "extra_in_model": extra,
                "candidate_transition": first_transition_touching_page(predicted_records, page),
            }
    return None


def first_transition_touching_page(records: list[dict[str, Any]], page: str) -> dict[str, Any] | None:
    """查找第一条触达指定 page 的 predicted transition。"""

    if not page:
        return None
    for record in records:
        pages = record.get("target_page_set")
        if not isinstance(pages, list) or page not in {str(item) for item in pages}:
            continue
        return {
            "source_fact_id": record.get("source_fact_id"),
            "source_event_index": record.get("source_event_index"),
            "source_event_name": record.get("source_event_name"),
            "request_id": record.get("request_id"),
            "operation_id": record.get("operation_id"),
            "transition_kind": record.get("transition_kind"),
            "predicted_operation_kind": record.get("predicted_operation_kind"),
            "decision_reason": record.get("decision_reason"),
        }
    return None


def false_like(value: Any) -> bool:
    """解析配置/JSON 中常见的 false 字符串表示。"""

    text = str(value).lower()
    return text in {"false", "0", "no", "off"}


def main(argv: list[str] | None = None) -> int:
    """CLI 入口：输出单次 modeling prediction JSON。"""

    require_modeling_container()
    prediction = run_from_cli(parse_args(argv))
    print(json.dumps(prediction, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
