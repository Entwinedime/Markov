#!/usr/bin/env python3
"""Modeling CLI。

本脚本只做编排：读取配置、调用 trace merger、启动 C++ TraceGraph 后端。
建模逻辑、DAG 构建、拓扑仿真和子模块执行都在 C++ 中完成。
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


from ..common.io import load_json, write_json
from ..common.paths import ROOT_DIR
from ..common.paths import resolve_repo_path as common_resolve_repo_path
from ..common.paths import running_in_modeling_container
from .cpp_config import trace_graph_executable, write_cpp_model_config
from .trace_inputs import prepare_trace_inputs
from .hicache_validation_artifacts import (
    build_validation,
    hicache_state_validation_enabled,
    write_hicache_predicted_state_trace_if_available,
    write_hicache_recommended_cpp_config_if_available,
)
from .workload import discover_workload_window


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
    emit_dag_analysis: bool
    debug: bool


def resolve_repo_path(value: str) -> Path:
    """把用户输入路径解析为仓库内真实路径。"""

    path = common_resolve_repo_path(value)
    if path is None:
        raise ValueError("expected a non-empty path")
    return path


def require_modeling_container() -> None:
    """阻止宿主机直接执行 modeling runner。"""

    if running_in_modeling_container():
        return
    raise SystemExit(
        "scripts/internal/entrypoints/model.py is container-internal. "
        "Use scripts/model.sh ... or scripts/run.sh modeling -- ... instead."
    )


def parse_args(argv: list[str] | None = None) -> ModelingOptions:
    """解析 modeling runner CLI 参数。"""

    parser = argparse.ArgumentParser(description="Run C++ trace-based modeling.")
    parser.add_argument("--config", required=True, help="modeling config path")
    parser.add_argument("--output-dir", help="override config.output_dir")
    parser.add_argument("--profile-manifest", help="override config.input.profile_manifest")
    parser.add_argument(
        "--cpp-model-config", help="override config.cpp_model_config with a ready-to-use C++ model config JSON"
    )
    parser.add_argument(
        "--hicache-oracle-trace",
        action="append",
        default=[],
        help="override validation.hicache_state.oracle_trace_paths; may be repeated",
    )
    parser.add_argument(
        "--mode", choices=("faithful_replay", "cache_state", "cache_patch"), help="override config.mode"
    )
    parser.add_argument("--emit-dag-chrome-trace", action="store_true", help="emit DAG as Chrome trace JSON")
    parser.add_argument("--emit-module-summary", action="store_true", help="emit C++ module summary JSON")
    parser.add_argument("--emit-validation", action="store_true", help="emit validation.json")
    parser.add_argument("--emit-dag-analysis", action="store_true", help="emit Debug-only DAG analysis artifacts")
    parser.add_argument("--debug", action="store_true", help="enable C++ TraceGraph debug logging")
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
        emit_dag_analysis=bool(args.emit_dag_analysis),
        debug=bool(args.debug),
    )


def run_from_cli(options: ModelingOptions) -> dict[str, Any]:
    """执行一次 modeling run。

    Python 侧只准备输入 trace、C++ model config 和输出路径；实际 DAG 构建、模块执行和拓扑仿真
    均由 C++ TraceGraph 完成。
    """

    config = load_json(options.config_path)
    mode = options.mode or str(config.get("mode") or "faithful_replay")
    output_dir = options.output_dir or resolve_repo_path(
        str(config.get("output_dir") or "data/modeling_runs/cpp_trace_graph")
    )
    output_dir.mkdir(parents=True, exist_ok=True)
    outputs_cfg = config.get("outputs") if isinstance(config.get("outputs"), dict) else {}
    emit_dag_chrome_trace = options.emit_dag_chrome_trace or bool(outputs_cfg.get("emit_dag_chrome_trace", False))
    emit_module_summary = options.emit_module_summary or bool(outputs_cfg.get("emit_module_summary", False))
    emit_validation = options.emit_validation or bool(outputs_cfg.get("emit_validation", False))
    emit_dag_analysis = options.emit_dag_analysis or bool(outputs_cfg.get("emit_dag_analysis", False))
    if emit_validation and hicache_state_validation_enabled(config):
        # HiCache state validation 依赖 C++ module summary 中的 state trace。
        require_validation_backend(config)
        emit_module_summary = True
    if emit_dag_analysis:
        require_validation_backend(config)
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
    if emit_dag_analysis:
        command.extend(["--dag-analysis-output-dir", str(output_dir)])
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
    predicted_state_trace = (
        write_hicache_predicted_state_trace_if_available(module_summary, output_dir) if emit_validation else None
    )
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
    if emit_dag_analysis:
        enrich_dag_quality(output_dir / "dag_quality.json", manifest_path, trace_paths, mode)
    return prediction


def require_validation_backend(config: dict[str, Any]) -> None:
    """HiCache validation 必须显式选择 Debug/validation C++ backend。"""

    cpp = config.get("cpp_trace_graph") if isinstance(config.get("cpp_trace_graph"), dict) else {}
    backend_kind = str(cpp.get("backend_kind") or "").strip().lower()
    if backend_kind == "validation" or cpp.get("require_debug") is True:
        return
    raise ValueError("Debug-only modeling outputs require cpp_trace_graph.backend_kind='validation'")


def enrich_dag_quality(
    dag_quality_path: Path,
    manifest_path: Path | None,
    trace_paths: list[Path],
    mode: str,
) -> None:
    """把 manifest 侧 channel coverage 补写进 C++ DAG quality artifact。"""

    dag_quality = load_json(dag_quality_path)
    manifest = load_json(manifest_path) if manifest_path is not None else {}
    profiling = manifest.get("profiling") if isinstance(manifest.get("profiling"), dict) else {}
    trace = manifest.get("trace") if isinstance(manifest.get("trace"), dict) else {}
    sidecar = manifest.get("sidecar") if isinstance(manifest.get("sidecar"), dict) else {}
    python_probe_files = (
        sidecar.get("python_probe_files") if isinstance(sidecar.get("python_probe_files"), list) else []
    )
    dag_quality["trace_channel_coverage"] = {
        "torch_trace_files": len(
            trace.get("torch_trace_files", []) if isinstance(trace.get("torch_trace_files"), list) else []
        ),
        "ld_preload_trace_files": len(
            trace.get("ld_preload_trace_files", []) if isinstance(trace.get("ld_preload_trace_files"), list) else []
        ),
        "python_probe_trace_files": len(python_probe_files),
        "requested_consumers": profiling.get("python_consumers", []),
        "selected_probe_target_count": None,
        "observed_probe_target_count": None,
        "input_trace_files": [str(path) for path in trace_paths],
    }
    dag_quality["run"] = {
        "run_id": manifest.get("run_id") or manifest.get("experiment_id"),
        "manifest": str(manifest_path) if manifest_path is not None else None,
        "config_path": manifest.get("config_path"),
        "mode": mode,
    }
    write_json(dag_quality_path, dag_quality)


def main(argv: list[str] | None = None) -> int:
    """CLI 入口：输出单次 modeling prediction JSON。"""

    effective_argv = sys.argv[1:] if argv is None else argv
    if any(arg in {"-h", "--help"} for arg in effective_argv):
        parse_args(effective_argv)
        return 0
    require_modeling_container()
    prediction = run_from_cli(parse_args(effective_argv))
    print(json.dumps(prediction, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
