#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import math
import re
import statistics
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager, nullcontext
from copy import deepcopy
from pathlib import Path
from typing import Any, Dict, Iterable, Iterator, List, Optional, Tuple


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "src/modeling"))

from trace_sim_model.hicache_radix_sim import NoRadixOpsError, RadixInputError, run_hicache_radix_sim


def read_json(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, data: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def resolve_path(path: str, base: Path) -> Path:
    candidate = Path(path).expanduser()
    if candidate.is_absolute():
        return candidate
    if (base / candidate).exists():
        return (base / candidate).resolve()
    return (REPO_ROOT / candidate).resolve()


def sanitize_name(name: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", name.strip())
    return safe.strip("._-") or "experiment"


def merge_tier_overrides(base_tiers: List[Dict[str, Any]], overrides: Dict[str, Any]) -> List[Dict[str, Any]]:
    merged = [deepcopy(tier) for tier in base_tiers]
    by_name = {str(tier.get("name")): tier for tier in merged if tier.get("name") is not None}
    for tier_name, tier_override in overrides.items():
        if tier_name not in by_name:
            new_tier = {"name": tier_name}
            merged.append(new_tier)
            by_name[tier_name] = new_tier
        deep_merge_into(by_name[tier_name], tier_override)
    return merged


def deep_merge_into(base: Dict[str, Any], overrides: Dict[str, Any]) -> Dict[str, Any]:
    for key, value in overrides.items():
        if key == "tiers" and isinstance(base.get(key), list) and isinstance(value, dict):
            base[key] = merge_tier_overrides(base[key], value)
        elif isinstance(base.get(key), dict) and isinstance(value, dict):
            deep_merge_into(base[key], value)
        else:
            base[key] = deepcopy(value)
    return base


def materialize_config(base_config: Dict[str, Any], *overrides: Dict[str, Any]) -> Dict[str, Any]:
    merged = deepcopy(base_config)
    for override in overrides:
        deep_merge_into(merged, override)
    return merged


def run_command(cmd: List[str], cmd_path: Path, *, check: bool = True) -> subprocess.CompletedProcess[str]:
    cmd_path.parent.mkdir(parents=True, exist_ok=True)
    cmd_path.write_text(" ".join(cmd) + "\n", encoding="utf-8")
    return subprocess.run(cmd, cwd=REPO_ROOT, text=True, check=check)


def expand_experiments(matrix: Dict[str, Any]) -> List[Dict[str, Any]]:
    experiments = matrix.get("experiments")
    if not isinstance(experiments, list) or not experiments:
        raise ValueError("matrix config must define a non-empty experiments list")
    expanded: List[Dict[str, Any]] = []
    for index, experiment in enumerate(experiments, start=1):
        if not isinstance(experiment, dict):
            raise TypeError(f"experiments[{index - 1}] must be an object")
        name = str(experiment.get("name", f"experiment-{index}"))
        expanded.append(
            {
                "index": index,
                "name": sanitize_name(name),
                "display_name": name,
                "run_id": f"{index:02d}_{sanitize_name(name)}",
                "fit_model_overrides": experiment.get("fit_model_overrides", {}),
            }
        )
    return expanded


def discover_traces(run_dir: Path) -> List[Path]:
    patterns = (
        "trace/merged/merged_trace.pid*.json",
        "trace/merged/merged_trace*.json",
    )
    seen = set()
    traces: List[Path] = []
    for pattern in patterns:
        for path in sorted(run_dir.glob(pattern)):
            if path.is_file() and path.name.startswith("merge_report."):
                continue
            if path.is_file() and path not in seen:
                seen.add(path)
                traces.append(path)
        if traces:
            return traces
    return []


def discover_python_probe_traces(run_dir: Path) -> List[Path]:
    patterns = (
        "trace/python_probe/python_probe_trace.rankunknown.pid*.json",
        "trace/python_probe/python_probe_trace.rank*.pid*.json",
    )
    seen = set()
    traces: List[Path] = []
    for pattern in patterns:
        for path in sorted(run_dir.glob(pattern)):
            if path.is_file() and path not in seen:
                seen.add(path)
                traces.append(path)
        if traces:
            return traces
    return []


def cleanup_merged_traces(run_dir: Path) -> int:
    removed = 0
    for path in sorted((run_dir / "trace" / "merged").glob("merged_trace*.json")):
        if path.is_file():
            path.unlink()
            removed += 1
    return removed


def ensure_merged(run_dir: Path, overwrite: bool) -> List[Path]:
    traces = discover_traces(run_dir)
    if traces and not overwrite:
        return traces
    cmd = [
        "python3",
        "scripts/trace/merge_all_traces.py",
        "--root",
        str(run_dir),
        "--overwrite",
    ]
    run_command(cmd, run_dir / "fit_prepare" / "merge_cmd.txt")
    return discover_traces(run_dir)


@contextmanager
def merged_trace_scope(run_dir: Path, overwrite: bool, cleanup_merged_traces_after: bool) -> Iterator[List[Path]]:
    traces = ensure_merged(run_dir, overwrite)
    if not traces:
        raise RuntimeError(f"no merged traces produced for {run_dir}")
    try:
        yield traces
    finally:
        if cleanup_merged_traces_after:
            cleanup_merged_traces(run_dir)


def inspect_run(run_dir: Path, overwrite: bool) -> Dict[str, Any]:
    output = run_dir / "model" / "hicache_inspect.json"
    if output.is_file() and not overwrite:
        return read_json(output)
    cmd = [
        "python3",
        "scripts/trace/inspect_hicache.py",
        str(run_dir),
        "--output",
        str(output),
    ]
    result = run_command(cmd, run_dir / "fit_prepare" / "inspect_cmd.txt", check=False)
    if not output.is_file():
        return {
            "whatif_readiness": {
                "latency_bandwidth_ready": False,
                "capacity_eviction_ready": False,
                "prefetch_policy_ready": False,
                "missing": ["hicache_inspect_output"],
            },
            "warning": f"inspect_hicache exited with {result.returncode}",
        }
    report = read_json(output)
    if result.returncode != 0:
        report.setdefault("warnings", []).append(f"inspect_hicache exited with {result.returncode}")
    return report


def merge_health(run_dir: Path) -> Dict[str, Any]:
    summary_path = run_dir / "trace" / "merged" / "merge_summary.json"
    reports: List[Dict[str, Any]] = []
    if summary_path.is_file():
        data = read_json(summary_path)
        raw_reports = data.get("reports", [])
        if isinstance(raw_reports, list):
            reports = [report for report in raw_reports if isinstance(report, dict)]
    if not reports:
        for path in sorted((run_dir / "trace" / "merged").glob("merge_report.pid*.json")):
            reports.append(read_json(path))

    total_unmatched = sum(int(report.get("unmatched", 0) or 0) for report in reports)
    sidecar_events = sum(int(report.get("sidecar_events_appended", 0) or 0) for report in reports)
    return {
        "report_count": len(reports),
        "success": bool(reports) and all(bool(report.get("success", False)) for report in reports),
        "total_unmatched": total_unmatched,
        "sidecar_events_appended": sidecar_events,
        "native_unmatched_zero": bool(reports) and total_unmatched == 0,
        "python_probe_sidecar_events": sidecar_events,
        "python_probe_sidecar_present": sidecar_events > 0,
    }


def run_radix_sim(
    traces: Iterable[Path],
    scenario_dir: Path,
    config_path: Path,
    scenario_name: str,
    overwrite: bool,
    keep_generated_trace: bool,
) -> Dict[str, Any]:
    summary_path = scenario_dir / "cache_io_summary.json"
    generated_trace_path = scenario_dir / "radix_sim_trace.json"
    run_summary_path = scenario_dir / "run_summary.json"
    if summary_path.is_file() and run_summary_path.is_file() and not overwrite:
        if not keep_generated_trace and generated_trace_path.is_file():
            generated_trace_path.unlink()
        return {
            "cache_io_summary": read_json(summary_path),
            "run_summary": read_json(run_summary_path),
            "summary_path": summary_path,
            "run_summary_path": run_summary_path,
            "generated_trace_path": generated_trace_path,
        }

    scenario_dir.mkdir(parents=True, exist_ok=True)
    model_config = read_json(config_path)
    try:
        run_hicache_radix_sim(
            traces,
            model_config,
            scenario_name=scenario_name,
            output_path=generated_trace_path,
            summary_path=summary_path,
            run_summary_path=run_summary_path,
        )
    except RadixInputError as exc:
        write_json(scenario_dir / "input_readiness.json", exc.readiness)
        raise
    except NoRadixOpsError:
        write_json(
            scenario_dir / "input_readiness.json",
            {
                "radix_sim_ready": False,
                "radix_op_events": 0,
                "rejected_reasons": {"missing:radix_op_model_input": 1},
            },
        )
        raise
    write_json(scenario_dir / "input_readiness.json", read_json(summary_path).get("input_readiness", {}))
    (scenario_dir / "radix_sim_cmd.txt").write_text(
        "radix_sim " + " ".join(str(trace) for trace in traces) + "\n",
        encoding="utf-8",
    )
    if not keep_generated_trace and generated_trace_path.is_file():
        generated_trace_path.unlink()
    return {
        "cache_io_summary": read_json(summary_path),
        "run_summary": read_json(run_summary_path),
        "summary_path": summary_path,
        "run_summary_path": run_summary_path,
        "generated_trace_path": generated_trace_path,
    }


def reuse_actual_radix_result(
    actual: Dict[str, Any],
    pair_dir: Path,
    config_path: Path,
    scenario_name: str,
) -> Dict[str, Any]:
    summary_path = pair_dir / "cache_io_summary.json"
    generated_trace_path = pair_dir / "radix_sim_trace.json"
    run_summary_path = pair_dir / "run_summary.json"
    pair_dir.mkdir(parents=True, exist_ok=True)

    write_json(summary_path, actual["cache_io_summary"])

    run_summary = deepcopy(actual["run_summary"])
    run_summary["scenario_name"] = scenario_name
    run_summary["output_file"] = str(generated_trace_path)
    run_summary["model_config"] = str(config_path)
    run_summary["model_summary"] = str(summary_path)
    write_json(run_summary_path, run_summary)
    (pair_dir / "radix_sim_cmd.txt").write_text("reused base actual summary for identical base and target\n", encoding="utf-8")
    if generated_trace_path.is_file():
        generated_trace_path.unlink()

    return {
        "cache_io_summary": read_json(summary_path),
        "run_summary": read_json(run_summary_path),
        "summary_path": summary_path,
        "run_summary_path": run_summary_path,
        "generated_trace_path": generated_trace_path,
    }


def workload_report_path(run_dir: Path) -> Optional[Path]:
    candidates = (
        run_dir / "workload" / "workload_report.json",
        run_dir / "bench" / "workload_report.json",
        run_dir / "workload_report.json",
    )
    for path in candidates:
        if path.is_file():
            return path
    matches = sorted(run_dir.glob("**/workload_report.json"))
    return matches[0] if matches else None


def workload_latency_ns(run_dir: Path) -> Tuple[float, Dict[str, Any]]:
    path = workload_report_path(run_dir)
    if path is None:
        return 0.0, {"path": None, "warning": "missing_workload_report"}
    report = read_json(path)
    selected = report.get("selected_latency", {})
    if isinstance(selected, dict) and selected.get("latency_ms_sum") is not None:
        return float(selected.get("latency_ms_sum", 0.0)) * 1_000_000.0, {"path": str(path), "selected_latency": selected}
    phases = report.get("phases", {})
    total_ms = 0.0
    for phase in ("reuse_A", "reuse_A_again"):
        value = phases.get(phase, {}) if isinstance(phases, dict) else {}
        total_ms += float(value.get("latency_ms_sum", 0.0) or 0.0)
    if total_ms > 0:
        return total_ms * 1_000_000.0, {"path": str(path), "selected_phases": ["reuse_A", "reuse_A_again"]}
    total = report.get("total", {})
    return float(total.get("latency_ms_sum", 0.0) or 0.0) * 1_000_000.0, {
        "path": str(path),
        "warning": "selected_latency_missing_using_total",
    }


def numeric(value: Any) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0.0


def map_delta(target: Dict[str, Any], base: Dict[str, Any]) -> Dict[str, float]:
    keys = set(target) | set(base)
    return {key: numeric(target.get(key)) - numeric(base.get(key)) for key in keys}


def scalar_delta(target: Dict[str, Any], base: Dict[str, Any], key: str) -> float:
    return numeric(target.get(key)) - numeric(base.get(key))


def normalized_error(predicted_delta: float, actual_delta: float, scale_values: Iterable[float]) -> float:
    scale = max([abs(actual_delta), 1.0, *[abs(value) for value in scale_values]])
    return abs(predicted_delta - actual_delta) / scale


def map_error(predicted_delta: Dict[str, float], actual_delta: Dict[str, float], target: Dict[str, Any], base: Dict[str, Any]) -> float:
    keys = set(predicted_delta) | set(actual_delta) | set(target) | set(base)
    if not keys:
        return 0.0
    errors = []
    for key in sorted(keys):
        errors.append(
            normalized_error(
                predicted_delta.get(key, 0.0),
                actual_delta.get(key, 0.0),
                (numeric(target.get(key)), numeric(base.get(key))),
            )
        )
    return statistics.fmean(errors)


def cache_error(predicted_target: Dict[str, Any], predicted_base: Dict[str, Any], actual_target: Dict[str, Any], actual_base: Dict[str, Any]) -> Dict[str, float]:
    pred_latency_delta = scalar_delta(predicted_target, predicted_base, "estimated_latency_us")
    actual_latency_delta = scalar_delta(actual_target, actual_base, "estimated_latency_us")
    latency_error = normalized_error(
        pred_latency_delta,
        actual_latency_delta,
        (numeric(actual_target.get("estimated_latency_us")), numeric(actual_base.get("estimated_latency_us"))),
    )
    bytes_error = map_error(
        map_delta(predicted_target.get("bytes_by_edge", {}), predicted_base.get("bytes_by_edge", {})),
        map_delta(actual_target.get("bytes_by_edge", {}), actual_base.get("bytes_by_edge", {})),
        actual_target.get("bytes_by_edge", {}),
        actual_base.get("bytes_by_edge", {}),
    )
    pages_error = map_error(
        map_delta(predicted_target.get("pages_by_edge", {}), predicted_base.get("pages_by_edge", {})),
        map_delta(actual_target.get("pages_by_edge", {}), actual_base.get("pages_by_edge", {})),
        actual_target.get("pages_by_edge", {}),
        actual_base.get("pages_by_edge", {}),
    )
    evict_error = map_error(
        map_delta(predicted_target.get("evictions_by_tier", {}), predicted_base.get("evictions_by_tier", {})),
        map_delta(actual_target.get("evictions_by_tier", {}), actual_base.get("evictions_by_tier", {})),
        actual_target.get("evictions_by_tier", {}),
        actual_base.get("evictions_by_tier", {}),
    )
    aggregate = 0.4 * latency_error + 0.2 * bytes_error + 0.2 * pages_error + 0.2 * evict_error
    return {
        "cache_error": aggregate,
        "cache_latency_error": latency_error,
        "cache_bytes_error": bytes_error,
        "cache_pages_error": pages_error,
        "cache_eviction_error": evict_error,
        "predicted_cache_latency_delta_us": pred_latency_delta,
        "actual_cache_latency_delta_us": actual_latency_delta,
    }


def sign(value: float) -> int:
    if value > 0:
        return 1
    if value < 0:
        return -1
    return 0


def percentile(values: List[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil((pct / 100.0) * len(ordered)) - 1))
    return ordered[index]


def readiness_from_inspect(report: Dict[str, Any]) -> Dict[str, Any]:
    readiness = report.get("whatif_readiness", {})
    if not isinstance(readiness, dict):
        readiness = {}
    return {
        "latency_bandwidth_ready": bool(readiness.get("latency_bandwidth_ready", False)),
        "capacity_eviction_ready": bool(readiness.get("capacity_eviction_ready", False)),
        "prefetch_policy_ready": bool(readiness.get("prefetch_policy_ready", False)),
        "observed_readback_ready": bool(readiness.get("observed_readback_ready", False)),
        "inferred_readback_ready": bool(readiness.get("inferred_readback_ready", False)),
        "policy_simulation_ready": bool(readiness.get("policy_simulation_ready", False)),
        "page_identity_ready": bool(readiness.get("page_identity_ready", False)),
        "page_identity_map_ready": bool(readiness.get("page_identity_map_ready", False)),
        "runtime_page_alias_ready": bool(readiness.get("runtime_page_alias_ready", False)),
        "state_scope_ready": bool(readiness.get("state_scope_ready", False)),
        "operation_lifecycle_ready": bool(readiness.get("operation_lifecycle_ready", False)),
        "load_back_link_ready": bool(readiness.get("load_back_link_ready", False)),
        "parent_prefix_ready": bool(readiness.get("parent_prefix_ready", False)),
        "radix_sim_ready": bool(readiness.get("radix_sim_ready", False)),
        "missing": readiness.get("missing", []),
    }


def write_csv(path: Path, rows: List[Dict[str, Any]]) -> None:
    fieldnames = [
        "base",
        "target",
        "predicted_cache_latency_delta_us",
        "actual_cache_latency_delta_us",
        "cache_latency_delta_sign_match",
        "cache_error",
        "cache_latency_error",
        "cache_bytes_error",
        "cache_pages_error",
        "cache_eviction_error",
        "predicted_e2e_delta_ns",
        "actual_e2e_delta_ns",
        "e2e_error",
        "weighted_score",
        "warnings",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: row.get(key, "") for key in fieldnames})


def main() -> int:
    parser = argparse.ArgumentParser(description="Evaluate HiCache base-to-target what-if fitting across a profile matrix.")
    parser.add_argument("--run-root", required=True, help="Profile matrix suite run root.")
    parser.add_argument("--matrix-config", required=True, help="Profile matrix JSON config.")
    parser.add_argument("--base-model-config", default="configs/modeling/hicache_ascend_file.json")
    parser.add_argument("--output-dir", help="Output directory. Defaults to <run-root>/fit.")
    parser.add_argument("--no-prepare", action="store_true", help="Skip merge and inspect; use existing merged traces.")
    parser.add_argument("--overwrite", action="store_true", help="Regenerate cached summaries.")
    parser.add_argument(
        "--base",
        action="append",
        help="Limit what-if prediction bases by experiment name. Repeat for multiple bases. Default uses all experiments.",
    )
    parser.add_argument(
        "--target",
        action="append",
        help="Limit what-if prediction targets by experiment name. Repeat for multiple targets. Default uses all experiments.",
    )
    parser.add_argument(
        "--pair-workers",
        type=int,
        default=1,
        help="Number of target what-if replays to run concurrently for each base config. Default: 1.",
    )
    parser.add_argument(
        "--cleanup-merged-traces",
        action="store_true",
        help="Delete merged_trace*.json files after each base run. Default keeps them for reuse.",
    )
    parser.add_argument(
        "--keep-generated-trace-output",
        action="store_true",
        help="Keep per-scenario RadixSim Chrome Trace outputs. Default removes them after summaries are written.",
    )
    parser.add_argument(
        "--cache-only-sidecar",
        action="store_true",
        help="Use python_probe sidecar traces directly for cache_io model iteration instead of full merged traces.",
    )
    args = parser.parse_args()

    run_root = resolve_path(args.run_root, Path.cwd())
    matrix_path = resolve_path(args.matrix_config, Path.cwd())
    base_config_path = resolve_path(args.base_model_config, Path.cwd())
    output_dir = resolve_path(args.output_dir, Path.cwd()) if args.output_dir else run_root / "fit"

    if not run_root.is_dir():
        raise SystemExit(f"run root not found: {run_root}")

    matrix = read_json(matrix_path)
    base_config = read_json(base_config_path)
    common_overrides = matrix.get("fit_model_base_overrides", {})
    experiments = expand_experiments(matrix)
    experiment_by_name = {experiment["name"]: experiment for experiment in experiments}
    base_names = set(args.base or experiment_by_name.keys())
    target_names = set(args.target or experiment_by_name.keys())
    missing_names = sorted((base_names | target_names) - set(experiment_by_name))
    if missing_names:
        raise SystemExit(f"unknown experiment name(s): {', '.join(missing_names)}")
    base_experiments = [experiment for experiment in experiments if experiment["name"] in base_names]
    target_experiments = [experiment for experiment in experiments if experiment["name"] in target_names]
    actual_experiments = [experiment for experiment in experiments if experiment["name"] in (base_names | target_names)]
    output_dir.mkdir(parents=True, exist_ok=True)
    write_json(output_dir / "matrix_config.json", matrix)

    actuals: Dict[str, Dict[str, Any]] = {}
    predictions: Dict[Tuple[str, str], Dict[str, Any]] = {}
    pair_root = output_dir / "pair_reports"

    for experiment in actual_experiments:
        name = experiment["name"]
        run_dir = run_root / experiment["run_id"]
        inspect_report: Dict[str, Any] = {"whatif_readiness": {"missing": ["inspect_skipped"]}}

        if args.cache_only_sidecar:
            traces = discover_python_probe_traces(run_dir)
            if not traces:
                raise SystemExit(f"no python_probe sidecar traces found for {name}: {run_dir}")
            trace_context = nullcontext(traces)
            inspect_report = inspect_run(run_dir, args.overwrite)
        elif args.no_prepare:
            traces = discover_traces(run_dir)
            if not traces:
                raise SystemExit(f"no merged traces found for {name}: {run_dir}")
            trace_context = nullcontext(traces)
        else:
            trace_context = merged_trace_scope(run_dir, args.overwrite, args.cleanup_merged_traces)

        with trace_context as traces:
            if not args.no_prepare:
                inspect_report = inspect_run(run_dir, args.overwrite)

            scenario_dir = run_dir / "model" / "actual"
            config = materialize_config(base_config, common_overrides, experiment.get("fit_model_overrides", {}))
            config_path = scenario_dir / "model_config.json"
            write_json(config_path, config)
            graph_result = run_radix_sim(
                traces,
                scenario_dir,
                config_path,
                f"actual:{name}",
                args.overwrite,
                args.keep_generated_trace_output,
            )
            latency_ns, workload = workload_latency_ns(run_dir)
            actuals[name] = {
                "experiment": experiment,
                "run_dir": str(run_dir),
                "cache_io_summary": graph_result["cache_io_summary"],
                "run_summary": graph_result["run_summary"],
                "workload_selected_latency_ns": latency_ns,
                "workload": workload,
                "readiness": readiness_from_inspect(inspect_report),
                "merge_health": merge_health(run_dir),
            }

            def compute_pair(target: Dict[str, Any]) -> Tuple[str, Dict[str, Any]]:
                target_name = target["name"]
                pair_name = f"{name}__{target_name}"
                pair_dir = pair_root / pair_name
                config = materialize_config(base_config, common_overrides, target.get("fit_model_overrides", {}))
                config_path = pair_dir / "scenario_config.json"
                write_json(config_path, config)
                if target_name == name:
                    result = reuse_actual_radix_result(actuals[name], pair_dir, config_path, f"{name}->{target_name}")
                else:
                    result = run_radix_sim(
                        traces,
                        pair_dir,
                        config_path,
                        f"{name}->{target_name}",
                        args.overwrite,
                        args.keep_generated_trace_output,
                    )
                return target_name, result

            if name in base_names:
                pair_workers = max(1, args.pair_workers)
                if pair_workers == 1:
                    for target in target_experiments:
                        target_name, result = compute_pair(target)
                        predictions[(name, target_name)] = result
                else:
                    with ThreadPoolExecutor(max_workers=pair_workers) as executor:
                        future_to_target = {executor.submit(compute_pair, target): target["name"] for target in target_experiments}
                        for future in as_completed(future_to_target):
                            target_name, result = future.result()
                            predictions[(name, target_name)] = result

    rows: List[Dict[str, Any]] = []

    for base in base_experiments:
        base_name = base["name"]
        base_actual = actuals[base_name]
        pred_base = predictions[(base_name, base_name)]["cache_io_summary"]
        pred_base_run = predictions[(base_name, base_name)]["run_summary"]
        for target in target_experiments:
            target_name = target["name"]
            target_actual = actuals[target_name]
            pred_target = predictions[(base_name, target_name)]["cache_io_summary"]
            pred_target_run = predictions[(base_name, target_name)]["run_summary"]

            cache_metrics = cache_error(
                pred_target,
                pred_base,
                target_actual["cache_io_summary"],
                base_actual["cache_io_summary"],
            )
            predicted_e2e_delta = scalar_delta(pred_target_run, pred_base_run, "simulated_e2e_ns")
            actual_e2e_delta = target_actual["workload_selected_latency_ns"] - base_actual["workload_selected_latency_ns"]
            e2e_error = normalized_error(
                predicted_e2e_delta,
                actual_e2e_delta,
                (target_actual["workload_selected_latency_ns"], base_actual["workload_selected_latency_ns"]),
            )
            weighted_score = 0.7 * cache_metrics["cache_error"] + 0.3 * e2e_error
            actual_sign = sign(cache_metrics["actual_cache_latency_delta_us"])
            predicted_sign = sign(cache_metrics["predicted_cache_latency_delta_us"])
            sign_match = None if actual_sign == 0 else actual_sign == predicted_sign
            warnings = sorted(
                set(
                    predictions[(base_name, target_name)]["cache_io_summary"].get("whatif_warnings", [])
                    + target_actual["readiness"].get("missing", [])
                    + base_actual["readiness"].get("missing", [])
                )
            )
            pair_report = {
                "base": base_name,
                "target": target_name,
                "base_run_dir": base_actual["run_dir"],
                "target_run_dir": target_actual["run_dir"],
                "predicted_cache_io": predictions[(base_name, target_name)]["summary_path"].as_posix(),
                "actual_target_cache_io": (Path(target_actual["run_dir"]) / "model" / "actual" / "cache_io_summary.json").as_posix(),
                "base_readiness": base_actual["readiness"],
                "target_readiness": target_actual["readiness"],
                "predicted_delta": {
                    "cache_latency_us": cache_metrics["predicted_cache_latency_delta_us"],
                    "simulated_e2e_ns": predicted_e2e_delta,
                },
                "actual_delta": {
                    "cache_latency_us": cache_metrics["actual_cache_latency_delta_us"],
                    "workload_selected_latency_ns": actual_e2e_delta,
                },
                "errors": {
                    **{key: value for key, value in cache_metrics.items() if key.endswith("_error") or key == "cache_error"},
                    "e2e_error": e2e_error,
                    "weighted_score": weighted_score,
                },
                "cache_latency_delta_sign_match": sign_match,
                "warnings": warnings,
            }
            pair_json = pair_root / f"{base_name}__{target_name}.json"
            write_json(pair_json, pair_report)
            rows.append(
                {
                    "base": base_name,
                    "target": target_name,
                    "cache_latency_delta_sign_match": "" if sign_match is None else str(sign_match).lower(),
                    "predicted_e2e_delta_ns": predicted_e2e_delta,
                    "actual_e2e_delta_ns": actual_e2e_delta,
                    "e2e_error": e2e_error,
                    "weighted_score": weighted_score,
                    "warnings": ";".join(warnings),
                    **cache_metrics,
                }
            )

    off_diagonal = [row for row in rows if row["base"] != row["target"]]
    scored = [float(row["weighted_score"]) for row in off_diagonal]
    sign_rows = [row for row in off_diagonal if row["cache_latency_delta_sign_match"] != ""]
    sign_accuracy = (
        sum(1 for row in sign_rows if row["cache_latency_delta_sign_match"] == "true") / len(sign_rows)
        if sign_rows
        else 0.0
    )
    readiness = {name: actuals[name]["readiness"] for name in actuals}
    merge_status = {name: actuals[name]["merge_health"] for name in actuals}
    fit_summary = {
        "run_root": str(run_root),
        "matrix_config": str(matrix_path),
        "base_model_config": str(base_config_path),
        "pair_count": len(rows),
        "off_diagonal_pair_count": len(off_diagonal),
        "cache_latency_delta_sign_accuracy": sign_accuracy,
        "median_weighted_score": statistics.median(scored) if scored else 0.0,
        "p90_weighted_score": percentile(scored, 90.0),
        "acceptance": {
            "ten_configs_profiled": len(experiments) == 10 and all(Path(actual["run_dir"]).is_dir() for actual in actuals.values()),
            "latency_bandwidth_ready_all": all(item["latency_bandwidth_ready"] for item in readiness.values()),
            "capacity_eviction_ready_all": all(item["capacity_eviction_ready"] for item in readiness.values()),
            "prefetch_policy_ready_at_least_8": sum(1 for item in readiness.values() if item["prefetch_policy_ready"]) >= 8,
            "cache_latency_delta_sign_accuracy_ge_80pct": sign_accuracy >= 0.8,
            "median_weighted_score_le_035": (statistics.median(scored) if scored else 0.0) <= 0.35,
            "p90_weighted_score_le_075": percentile(scored, 90.0) <= 0.75,
        },
        "readiness": readiness,
        "merge_status": merge_status,
        "base_experiments": [experiment["name"] for experiment in base_experiments],
        "target_experiments": [experiment["name"] for experiment in target_experiments],
        "experiments": [
            {
                "name": experiment["name"],
                "run_dir": actuals[experiment["name"]]["run_dir"],
                "workload_selected_latency_ns": actuals[experiment["name"]]["workload_selected_latency_ns"],
                "merge_health": actuals[experiment["name"]]["merge_health"],
            }
            for experiment in actual_experiments
        ],
    }
    fit_summary["acceptance"]["native_merge_unmatched_zero_all"] = all(
        item["native_unmatched_zero"] for item in merge_status.values()
    )
    fit_summary["acceptance"]["python_probe_sidecar_events_all"] = all(
        item["python_probe_sidecar_present"] for item in merge_status.values()
    )
    write_json(output_dir / "fit_summary.json", fit_summary)
    write_csv(output_dir / "fit_matrix.csv", rows)
    print(f"wrote {output_dir / 'fit_summary.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
