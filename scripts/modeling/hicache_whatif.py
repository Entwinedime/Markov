#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from copy import deepcopy
from pathlib import Path
from typing import Any, Dict, Iterable, List


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "src/modeling"))

from trace_sim_model.hicache_radix_sim import NoRadixOpsError, RadixInputError, run_hicache_radix_sim


def read_json(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, data: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def resolve_path(path: str, base: Path) -> Path:
    candidate = Path(path)
    if candidate.is_absolute():
        return candidate
    if (base / candidate).exists():
        return (base / candidate).resolve()
    return (REPO_ROOT / candidate).resolve()


def sanitize_name(name: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", name.strip())
    return safe.strip("._") or "scenario"


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


def run_radix_sim(
    traces: Iterable[Path],
    scenario_dir: Path,
    config_path: Path,
    scenario_name: str,
) -> Dict[str, Any]:
    summary_path = scenario_dir / "cache_io_summary.json"
    graph_path = scenario_dir / "radix_sim_trace.json"
    run_summary_path = scenario_dir / "run_summary.json"
    model_config = read_json(config_path)
    try:
        result = run_hicache_radix_sim(
            traces,
            model_config,
            scenario_name=scenario_name,
            output_path=graph_path,
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
    run_summary = result["run_summary"]
    cache_summary = result["cache_io_summary"]
    write_json(scenario_dir / "input_readiness.json", cache_summary.get("input_readiness", {}))
    return {
        "name": scenario_name,
        "directory": str(scenario_dir),
        "scenario_config": str(config_path),
        "cache_io_summary": str(summary_path),
        "generated_trace": str(graph_path),
        "run_summary": str(run_summary_path),
        "simulated_e2e_ns": run_summary.get("simulated_e2e_ns", 0),
        "cache_io_estimated_latency_us": cache_summary.get("estimated_latency_us", 0),
        "foreground_cache_io_us": cache_summary.get("foreground_cache_io_us", 0),
        "background_cache_io_us": cache_summary.get("background_cache_io_us", 0),
        "movement_events_used": cache_summary.get("movement_events_used", 0),
        "transfer_events": cache_summary.get("transfer_events", 0),
        "eviction_events": cache_summary.get("eviction_events", 0),
        "evictions_by_tier": cache_summary.get("evictions_by_tier", {}),
        "miss_pages_by_tier": cache_summary.get("miss_pages_by_tier", {}),
        "warnings": sorted(set(cache_summary.get("whatif_warnings", []) + run_summary.get("warnings", []))),
    }


def write_csv(path: Path, rows: List[Dict[str, Any]]) -> None:
    fieldnames = [
        "name",
        "simulated_e2e_ns",
        "delta_e2e_ns",
        "cache_io_estimated_latency_us",
        "delta_cache_io_estimated_latency_us",
        "foreground_cache_io_us",
        "background_cache_io_us",
        "movement_events_used",
        "transfer_events",
        "eviction_events",
        "warnings",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: row.get(key, "") for key in fieldnames})


def main() -> int:
    parser = argparse.ArgumentParser(description="Run explicit HiCache what-if scenarios against one base trace set.")
    parser.add_argument("--suite", required=True, help="What-if suite JSON.")
    parser.add_argument("--trace", action="append", required=True, help="Merged trace input. Repeat for multiple ranks/PIDs.")
    parser.add_argument("--output-dir", required=True, help="Directory for scenario outputs.")
    args = parser.parse_args()

    suite_path = resolve_path(args.suite, Path.cwd())
    suite = read_json(suite_path)
    base_config_path = resolve_path(suite["base_model_config"], suite_path.parent)
    base_config = read_json(base_config_path)
    base_overrides = suite.get("base_overrides", {})
    scenarios = suite.get("scenarios", [])
    if not scenarios:
        raise SystemExit("what-if suite must define at least one scenario")

    traces = [resolve_path(trace, Path.cwd()) for trace in args.trace]
    missing = [str(trace) for trace in traces if not trace.exists()]
    if missing:
        raise SystemExit(f"trace input not found: {', '.join(missing)}")

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    write_json(output_dir / "suite_config.json", suite)

    rows: List[Dict[str, Any]] = []
    for index, scenario in enumerate(scenarios):
        name = str(scenario.get("name", f"scenario_{index:02d}"))
        scenario_dir = output_dir / f"{index:02d}_{sanitize_name(name)}"
        scenario_dir.mkdir(parents=True, exist_ok=True)
        scenario_config = materialize_config(base_config, base_overrides, scenario.get("overrides", {}))
        scenario_config_path = scenario_dir / "scenario_config.json"
        write_json(scenario_config_path, scenario_config)
        rows.append(run_radix_sim(traces, scenario_dir, scenario_config_path, name))

    baseline = rows[0]
    baseline_e2e = int(baseline.get("simulated_e2e_ns", 0) or 0)
    baseline_cache = int(baseline.get("cache_io_estimated_latency_us", 0) or 0)
    for row in rows:
        row["delta_e2e_ns"] = int(row.get("simulated_e2e_ns", 0) or 0) - baseline_e2e
        row["delta_cache_io_estimated_latency_us"] = int(row.get("cache_io_estimated_latency_us", 0) or 0) - baseline_cache
        row["warnings"] = ";".join(row.get("warnings", []))

    aggregate = {
        "suite": str(suite_path),
        "base_model_config": str(base_config_path),
        "input_traces": [str(trace) for trace in traces],
        "baseline": baseline["name"],
        "engine": "radix_sim",
        "scenarios": rows,
    }
    write_json(output_dir / "whatif_summary.json", aggregate)
    write_csv(output_dir / "whatif_summary.csv", rows)
    print(f"wrote {output_dir / 'whatif_summary.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
