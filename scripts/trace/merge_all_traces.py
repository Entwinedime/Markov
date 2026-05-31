#!/usr/bin/env python3
import argparse
import glob
import json
import logging
import os
import re
import sys
from typing import Dict, Iterable, List, Optional, Tuple

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from trace_merger import merge_traces

logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")

CPU_TRACE_RE = re.compile(r"cpu_trace\.rankunknown\.pid(\d+)\.json$")
PYTHON_PROBE_TRACE_RE = re.compile(r"python_probe_trace\.rankunknown\.pid(\d+)\.json$")

def iter_experiment_dirs(root_dir: str) -> Iterable[str]:
    if os.path.isdir(os.path.join(root_dir, "trace")):
        yield root_dir
    for name in sorted(os.listdir(root_dir)):
        path = os.path.join(root_dir, name)
        if os.path.isdir(path):
            yield path

def extract_pid_from_cpu_trace(path: str) -> Optional[str]:
    match = CPU_TRACE_RE.search(os.path.basename(path))
    return match.group(1) if match else None

def extract_pid_from_profiler_path(path: str) -> Optional[str]:
    # Example: .../nb-xxxx-0_59774_20260502164827631_ascend_pt/ASCEND_PROFILER_OUTPUT/trace_view.json
    parts = os.path.normpath(path).split(os.sep)
    if len(parts) < 3:
        return None
    parent_name = parts[-3]
    for token in parent_name.split("_"):
        if token.isdigit():
            return token
    return None

def build_profiler_map(trace_dir: str) -> Dict[str, str]:
    profiler_glob = os.path.join(trace_dir, "torch", "**", "ASCEND_PROFILER_OUTPUT", "trace_view.json")
    profiler_paths = glob.glob(profiler_glob, recursive=True)
    profiler_map: Dict[str, str] = {}

    for path in profiler_paths:
        pid = extract_pid_from_profiler_path(path)
        if not pid:
            logging.warning("Skip profiler trace with unknown pid: %s", path)
            continue
        if pid in profiler_map:
            logging.warning("Duplicate profiler pid %s. Keeping first: %s", pid, profiler_map[pid])
            continue
        profiler_map[pid] = path

    return profiler_map

def find_cpu_traces(trace_dir: str) -> List[str]:
    pattern = os.path.join(trace_dir, "cpu_trace.rankunknown.pid*.json")
    return sorted(glob.glob(pattern))

def build_python_probe_map(trace_dir: str) -> Dict[str, List[str]]:
    pattern = os.path.join(trace_dir, "python_probe", "python_probe_trace.rankunknown.pid*.json")
    probe_map: Dict[str, List[str]] = {}
    for path in sorted(glob.glob(pattern)):
        match = PYTHON_PROBE_TRACE_RE.search(os.path.basename(path))
        if not match:
            logging.warning("Skip python probe trace with unknown pid: %s", path)
            continue
        probe_map.setdefault(match.group(1), []).append(path)
    return probe_map

def merge_experiment(
    exp_dir: str,
    tolerance_us: float,
    window: int,
    margin_us: float,
    mode: str,
    overwrite: bool,
) -> Tuple[int, int]:
    trace_dir = os.path.join(exp_dir, "trace")
    if not os.path.isdir(trace_dir):
        return 0, 0

    cpu_traces = find_cpu_traces(trace_dir)
    if not cpu_traces:
        logging.info("No cpu traces found under %s", trace_dir)
        return 0, 0

    profiler_map = build_profiler_map(trace_dir)
    if not profiler_map:
        logging.warning("No profiler traces found under %s", trace_dir)
        return 0, 0
    python_probe_map = build_python_probe_map(trace_dir)

    out_dir = os.path.join(trace_dir, "merged")
    os.makedirs(out_dir, exist_ok=True)

    total = 0
    merged = 0
    reports: List[Dict[str, object]] = []

    for cpu_trace in cpu_traces:
        pid = extract_pid_from_cpu_trace(cpu_trace)
        if not pid:
            logging.warning("Skip cpu trace with unknown pid: %s", cpu_trace)
            continue

        profiler_trace = profiler_map.get(pid)
        if not profiler_trace:
            logging.warning("No profiler trace found for pid %s in %s", pid, exp_dir)
            continue

        out_path = os.path.join(out_dir, f"merged_trace.pid{pid}.json")
        report_path = os.path.join(out_dir, f"merge_report.pid{pid}.json")
        if os.path.exists(out_path) and not overwrite:
            logging.info("Skip existing merged trace: %s", out_path)
            continue

        total += 1
        logging.info("Merging pid %s -> %s", pid, out_path)
        report = merge_traces(
            profiler_path=profiler_trace,
            custom_path=cpu_trace,
            out_path=out_path,
            tolerance_us=tolerance_us,
            search_window=window,
            margin_us=margin_us,
            mode=mode,
            sidecar_paths=python_probe_map.get(pid, []),
            report_path=report_path,
        )
        reports.append(report)
        merged += 1

    if reports:
        summary_path = os.path.join(out_dir, "merge_summary.json")
        with open(summary_path, "w", encoding="utf-8") as summary_file:
            json.dump({"experiment": exp_dir, "reports": reports}, summary_file, indent=2, sort_keys=True)

    return total, merged

def main() -> int:
    parser = argparse.ArgumentParser(description="Merge cpu hook traces with profiler traces for each experiment.")
    parser.add_argument("--root", required=True, help="Root dir, e.g. /root/sglang/auto-profile-logs/20260502")
    parser.add_argument("--tolerance", type=float, default=10000.0, help="Max time diff in microseconds")
    parser.add_argument("--window", type=int, default=5, help="Binary search neighbor window size")
    parser.add_argument("--margin", type=float, default=100.0, help="Margin in microseconds before earliest profiler event")
    parser.add_argument("--mode", type=str, choices=["search", "sequential"], default="search")
    parser.add_argument("--overwrite", action="store_true", help="Overwrite existing merged traces")

    args = parser.parse_args()

    if not os.path.isdir(args.root):
        logging.error("Root dir does not exist: %s", args.root)
        return 1

    total = 0
    merged = 0

    for exp_dir in iter_experiment_dirs(args.root):
        t, m = merge_experiment(
            exp_dir=exp_dir,
            tolerance_us=args.tolerance,
            window=args.window,
            margin_us=args.margin,
            mode=args.mode,
            overwrite=args.overwrite,
        )
        total += t
        merged += m

    logging.info("Done. Merged %d/%d traces.", merged, total)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
