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


ROOT_DIR = Path(__file__).resolve().parents[2]
CONTAINER_REPO_PREFIXES = ("/workspace/trace-sim", "/opt/trace-sim")


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
    mode: str | None
    emit_dag_chrome_trace: bool
    emit_module_summary: bool
    emit_validation: bool
    debug: bool


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def resolve_repo_path(value: str) -> Path:
    path = Path(value).expanduser()
    if path.is_absolute():
        return map_repo_path(path)
    return ROOT_DIR / path


def map_repo_path(path: Path) -> Path:
    raw = str(path)
    for prefix in CONTAINER_REPO_PREFIXES:
        if raw == prefix:
            return ROOT_DIR
        if raw.startswith(prefix + "/"):
            return ROOT_DIR / raw[len(prefix) + 1 :]
    return path


def parse_args(argv: list[str] | None = None) -> ModelingOptions:
    parser = argparse.ArgumentParser(description="Run C++ trace-based modeling.")
    parser.add_argument("--config", required=True, help="modeling config path")
    parser.add_argument("--output-dir", help="override config.output_dir")
    parser.add_argument("--profile-manifest", help="override config.input.profile_manifest")
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
        mode=args.mode,
        emit_dag_chrome_trace=bool(args.emit_dag_chrome_trace),
        emit_module_summary=bool(args.emit_module_summary),
        emit_validation=bool(args.emit_validation),
        debug=bool(args.debug),
    )


def run_from_cli(options: ModelingOptions) -> dict[str, Any]:
    config = load_json(options.config_path)
    mode = options.mode or str(config.get("mode") or "faithful_replay")
    output_dir = options.output_dir or resolve_repo_path(str(config.get("output_dir") or "data/modeling_runs/cpp_trace_graph"))
    output_dir.mkdir(parents=True, exist_ok=True)
    outputs_cfg = config.get("outputs") if isinstance(config.get("outputs"), dict) else {}
    emit_dag_chrome_trace = options.emit_dag_chrome_trace or bool(outputs_cfg.get("emit_dag_chrome_trace", False))
    emit_module_summary = options.emit_module_summary or bool(outputs_cfg.get("emit_module_summary", False))
    emit_validation = options.emit_validation or bool(outputs_cfg.get("emit_validation", False))
    debug = options.debug or bool(outputs_cfg.get("debug", False))

    input_cfg = config.get("input") if isinstance(config.get("input"), dict) else {}
    manifest_path = options.profile_manifest
    if manifest_path is None and isinstance(input_cfg.get("profile_manifest"), str):
        manifest_path = resolve_repo_path(input_cfg["profile_manifest"])

    trace_paths = prepare_trace_inputs(config, input_cfg, manifest_path, output_dir)
    model_config_path = write_cpp_model_config(config, output_dir, mode)
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
        raise RuntimeError(f"C++ TraceGraph failed: {completed.stderr.strip() or completed.stdout.strip()}")

    summary = load_json(run_summary)
    prediction = {"predicted_e2e_ns": int(summary.get("simulated_e2e_ns", 0))}
    write_json(output_dir / "prediction.json", prediction)
    if emit_validation:
        write_json(output_dir / "validation.json", build_validation(mode, prediction, summary, workload_window, trace_paths, config))
    return prediction


def prepare_trace_inputs(config: dict[str, Any], input_cfg: dict[str, Any], manifest_path: Path | None, output_dir: Path) -> list[Path]:
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
    hicache = hicache_config_from_modules(config)
    modules: list[str] = []
    generated: dict[str, Any] = {"modules": modules}
    if node_scale is not None:
        modules.append("node_scale")
        generated["node_scale"] = node_scale
    if hicache is not None:
        modules.append("hicache")
        generated["hicache"] = hicache
    if not modules:
        return None
    path = output_dir / "cpp_model_config.json"
    write_json(path, generated)
    return path


def node_scale_config_from_modules(config: dict[str, Any]) -> dict[str, Any] | None:
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
    for module in config.get("modules") or []:
        if not isinstance(module, dict) or not module.get("enabled", True):
            continue
        name = str(module.get("name") or "").replace("-", "_").lower()
        if name not in {"hicachemodule", "hicache"}:
            continue
        module_cfg = dict(module.get("config") or {})
        hicache = dict(module_cfg.get("hicache") or module_cfg)
        return {"enabled": bool(hicache.get("enabled", True))}
    return None


def discover_workload_window(input_cfg: dict[str, Any], manifest_path: Path | None) -> WorkloadWindow | None:
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
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def trace_graph_executable(config: dict[str, Any]) -> Path:
    cpp = config.get("cpp_trace_graph") if isinstance(config.get("cpp_trace_graph"), dict) else {}
    if isinstance(cpp.get("executable"), str):
        executable = resolve_repo_path(cpp["executable"])
        if executable.is_file():
            return executable
    for path in (ROOT_DIR / "build/bin/trace_graph", ROOT_DIR / "build/src/modeling/trace_graph/trace_graph"):
        if path.is_file():
            return path
    raise FileNotFoundError("missing trace_graph executable; run cmake --build build --target trace_graph")


def build_validation(
    mode: str,
    prediction: dict[str, Any],
    run_summary: dict[str, Any],
    workload_window: WorkloadWindow | None,
    trace_paths: list[Path],
    config: dict[str, Any],
) -> dict[str, Any]:
    validation_cfg = config.get("validation") if isinstance(config.get("validation"), dict) else {}
    threshold = float(validation_cfg.get("faithful_replay_full_e2e_rel_error_max", 0.05))
    summary_real = optional_float(run_summary.get("real_e2e_ns"))
    actual = int(summary_real) if summary_real and summary_real > 0 else None
    predicted = int(prediction["predicted_e2e_ns"])
    rel_error = abs(predicted - actual) / actual if actual else None
    errors: list[str] = []
    if actual is None:
        errors.append("missing_trace_real_e2e")
    if actual and rel_error is not None and rel_error > threshold:
        errors.append("faithful_replay_full_e2e_error_too_high")
    return {
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


def main(argv: list[str] | None = None) -> int:
    prediction = run_from_cli(parse_args(argv))
    print(json.dumps(prediction, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
