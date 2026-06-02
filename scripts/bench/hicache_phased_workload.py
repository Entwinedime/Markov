#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import statistics
import time
import urllib.request
from pathlib import Path
from typing import Any, Dict, Iterable, List


PHASE_ORDER = ("warmup", "fill_A", "pressure_B", "reuse_A", "reuse_A_again")


def now_ms() -> float:
    return time.perf_counter() * 1000.0


def wall_ms() -> float:
    return time.time() * 1000.0


def make_shared_prefix(label: str, repeat: int) -> str:
    unit = f"HiCache calibration {label} shared prefix block. "
    return unit * repeat


def make_prompt(prefix: str, family: str, index: int, suffix_repeat: int) -> str:
    suffix = f" request family {family} item {index}. " * suffix_repeat
    return prefix + suffix


def request_generate(url: str, prompt: str, max_new_tokens: int, timeout_sec: int) -> Dict[str, Any]:
    body = {
        "text": prompt,
        "sampling_params": {
            "max_new_tokens": max_new_tokens,
            "temperature": 0,
            "ignore_eos": True,
        },
    }
    request = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    start_monotonic = now_ms()
    start_wall = wall_ms()
    try:
        with urllib.request.urlopen(request, timeout=timeout_sec) as response:
            payload = response.read()
        end_monotonic = now_ms()
        return {
            "status": "ok",
            "http_status": response.status,
            "response_bytes": len(payload),
            "start_time_ms": start_wall,
            "end_time_ms": wall_ms(),
            "latency_ms": end_monotonic - start_monotonic,
        }
    except Exception as exc:
        end_monotonic = now_ms()
        return {
            "status": "error",
            "error": type(exc).__name__,
            "error_message": str(exc),
            "response_bytes": 0,
            "start_time_ms": start_wall,
            "end_time_ms": wall_ms(),
            "latency_ms": end_monotonic - start_monotonic,
        }


def phase_stats(rows: Iterable[Dict[str, Any]]) -> Dict[str, Any]:
    rows = list(rows)
    latencies = [float(row["latency_ms"]) for row in rows]
    ok = [row for row in rows if row.get("status") == "ok"]
    if not latencies:
        return {"requests": 0, "ok": 0, "errors": 0, "latency_ms_sum": 0.0}
    return {
        "requests": len(rows),
        "ok": len(ok),
        "errors": len(rows) - len(ok),
        "latency_ms_sum": sum(latencies),
        "latency_ms_mean": statistics.fmean(latencies),
        "latency_ms_min": min(latencies),
        "latency_ms_max": max(latencies),
        "latency_ms_p50": statistics.median(latencies),
    }


def build_plan(args: argparse.Namespace) -> List[Dict[str, Any]]:
    prefix_a = make_shared_prefix("A", args.shared_prefix_repeat)
    prefix_b = make_shared_prefix("B", args.shared_prefix_repeat)
    fill_prompts = [make_prompt(prefix_a, "A", index, args.unique_suffix_repeat) for index in range(args.fill_requests)]
    pressure_prompts = [make_prompt(prefix_b, "B", index, args.unique_suffix_repeat) for index in range(args.pressure_requests)]

    plan: List[Dict[str, Any]] = []
    for index in range(args.warmup_requests):
        plan.append({"phase": "warmup", "prompt_id": f"warmup_{index}", "prompt": make_prompt(prefix_a, "warmup", index, 1)})
    for index, prompt in enumerate(fill_prompts):
        plan.append({"phase": "fill_A", "prompt_id": f"A_{index}", "prompt": prompt})
    for index, prompt in enumerate(pressure_prompts):
        plan.append({"phase": "pressure_B", "prompt_id": f"B_{index}", "prompt": prompt})
    for index in range(args.reuse_requests):
        source_index = index % max(1, len(fill_prompts))
        plan.append({"phase": "reuse_A", "prompt_id": f"A_{source_index}", "prompt": fill_prompts[source_index]})
    for index in range(args.reuse_again_requests):
        source_index = index % max(1, len(fill_prompts))
        plan.append({"phase": "reuse_A_again", "prompt_id": f"A_{source_index}", "prompt": fill_prompts[source_index]})
    return plan


def write_outputs(output_dir: Path, rows: List[Dict[str, Any]], args: argparse.Namespace) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    by_phase: Dict[str, List[Dict[str, Any]]] = {phase: [] for phase in PHASE_ORDER}
    for row in rows:
        by_phase.setdefault(str(row["phase"]), []).append(row)

    phases = {phase: phase_stats(by_phase.get(phase, [])) for phase in PHASE_ORDER}
    total = phase_stats(rows)
    selected = rows_for_selected_latency(rows)
    summary = {
        "args": vars(args),
        "phases": phases,
        "total": total,
        "selected_latency": phase_stats(selected),
        "selected_phases": ["reuse_A", "reuse_A_again"],
        "requests": rows,
    }
    (output_dir / "workload_report.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    with (output_dir / "workload_report.jsonl").open("w", encoding="utf-8") as fh:
        for row in rows:
            fh.write(json.dumps(row, sort_keys=True) + "\n")


def rows_for_selected_latency(rows: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    return [row for row in rows if row.get("phase") in {"reuse_A", "reuse_A_again"}]


def main() -> int:
    parser = argparse.ArgumentParser(description="Run deterministic phased requests to exercise SGLang HiCache movement paths.")
    parser.add_argument("--base-url", default="http://127.0.0.1:30001", help="SGLang server base URL.")
    parser.add_argument("--output-dir", required=True, help="Directory that will receive workload_report.json/jsonl.")
    parser.add_argument("--warmup-requests", type=int, default=1)
    parser.add_argument("--fill-requests", type=int, default=8)
    parser.add_argument("--pressure-requests", type=int, default=32)
    parser.add_argument("--reuse-requests", type=int, default=8)
    parser.add_argument("--reuse-again-requests", type=int, default=8)
    parser.add_argument("--shared-prefix-repeat", type=int, default=96)
    parser.add_argument("--unique-suffix-repeat", type=int, default=8)
    parser.add_argument("--max-new-tokens", type=int, default=8)
    parser.add_argument("--timeout-sec", type=int, default=600)
    parser.add_argument("--sleep-sec", type=float, default=0.0)
    args = parser.parse_args()

    url = args.base_url.rstrip("/") + "/generate"
    rows: List[Dict[str, Any]] = []
    for sequence_id, item in enumerate(build_plan(args)):
        result = request_generate(url, item["prompt"], args.max_new_tokens, args.timeout_sec)
        row = {
            "sequence_id": sequence_id,
            "phase": item["phase"],
            "prompt_id": item["prompt_id"],
            "prompt_chars": len(item["prompt"]),
            "max_new_tokens": args.max_new_tokens,
            **result,
        }
        rows.append(row)
        print(
            f"phase={row['phase']} seq={sequence_id} prompt_id={row['prompt_id']} status={row['status']} latency_ms={row['latency_ms']:.3f}",
            flush=True,
        )
        if args.sleep_sec > 0:
            time.sleep(args.sleep_sec)

    write_outputs(Path(args.output_dir), rows, args)
    return 0 if all(row["status"] == "ok" for row in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
