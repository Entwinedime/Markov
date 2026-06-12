#!/usr/bin/env python3
"""Compare front-door HiCache workload reports for cross-run diagnosis.

This helper deliberately stays above the HiCache state model. It answers a
narrow question: did the benchmark driver send the same request-shaped workload
to two runs? A positive result here does not mean the HiCache invariant input
streams are aligned; it only rules out a simpler front-door workload mismatch.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


REQUEST_SHAPE_ARG_KEYS = (
    "warmup_requests",
    "seed_requests",
    "reuse_requests",
    "backup_wait_requests",
    "pressure_requests",
    "pressure_unique_prefix",
    "reuse_after_pressure_requests",
    "prefetch_seed_requests",
    "prefetch_reuse_requests",
    "dirty_eviction_requests",
    "shared_prefix_repeat",
    "unique_suffix_repeat",
    "max_new_tokens",
)

REQUEST_IDENTITY_KEYS = (
    "sequence_id",
    "phase",
    "prompt_id",
    "prompt_chars",
    "max_new_tokens",
)

RESPONSE_OBSERVED_KEYS = (
    "status",
    "http_status",
    "response_bytes",
)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-report", type=Path, required=True)
    parser.add_argument("--target-report", type=Path, required=True)
    parser.add_argument("--source-label", default="source")
    parser.add_argument("--target-label", default="target")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--sample", type=int, default=8)
    return parser.parse_args(argv)


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def selected_args(report: dict[str, Any], keys: tuple[str, ...]) -> dict[str, Any]:
    args = report.get("args") if isinstance(report.get("args"), dict) else {}
    return {key: args.get(key) for key in keys}


def request_projection(row: Any, keys: tuple[str, ...]) -> dict[str, Any]:
    if not isinstance(row, dict):
        return {}
    return {key: row.get(key) for key in keys}


def request_sequence(report: dict[str, Any], keys: tuple[str, ...]) -> list[dict[str, Any]]:
    requests = report.get("requests") if isinstance(report.get("requests"), list) else []
    return [request_projection(row, keys) for row in requests]


def first_sequence_mismatch(left: list[dict[str, Any]], right: list[dict[str, Any]]) -> dict[str, Any] | None:
    limit = min(len(left), len(right))
    for index in range(limit):
        if left[index] != right[index]:
            return {
                "index": index,
                "source": left[index],
                "target": right[index],
                "differing_fields": sorted({key for key in set(left[index]) | set(right[index]) if left[index].get(key) != right[index].get(key)}),
            }
    if len(left) != len(right):
        return {
            "index": limit,
            "source": left[limit] if len(left) > limit else None,
            "target": right[limit] if len(right) > limit else None,
            "differing_fields": ["request_count"],
        }
    return None


def sequence_diff_samples(left: list[dict[str, Any]], right: list[dict[str, Any]], sample: int) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    limit = max(len(left), len(right))
    for index in range(limit):
        source = left[index] if index < len(left) else None
        target = right[index] if index < len(right) else None
        if source == target:
            continue
        differing = []
        if isinstance(source, dict) and isinstance(target, dict):
            differing = sorted({key for key in set(source) | set(target) if source.get(key) != target.get(key)})
        else:
            differing = ["request_count"]
        rows.append({"index": index, "source": source, "target": target, "differing_fields": differing})
        if len(rows) >= sample:
            break
    return rows


def phase_counts(report: dict[str, Any]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for row in report.get("requests") if isinstance(report.get("requests"), list) else []:
        if not isinstance(row, dict):
            continue
        phase = str(row.get("phase") or "")
        counts[phase] = counts.get(phase, 0) + 1
    return dict(sorted(counts.items()))


def build_report(args: argparse.Namespace) -> dict[str, Any]:
    source = load_json(args.source_report)
    target = load_json(args.target_report)
    if not isinstance(source, dict) or not isinstance(target, dict):
        raise ValueError("workload reports must be JSON objects")

    source_shape_args = selected_args(source, REQUEST_SHAPE_ARG_KEYS)
    target_shape_args = selected_args(target, REQUEST_SHAPE_ARG_KEYS)
    source_requests = request_sequence(source, REQUEST_IDENTITY_KEYS)
    target_requests = request_sequence(target, REQUEST_IDENTITY_KEYS)
    source_responses = request_sequence(source, RESPONSE_OBSERVED_KEYS)
    target_responses = request_sequence(target, RESPONSE_OBSERVED_KEYS)

    request_shape_args_match = source_shape_args == target_shape_args
    request_sequence_mismatch = first_sequence_mismatch(source_requests, target_requests)
    request_sequence_match = request_sequence_mismatch is None
    response_observation_mismatch = first_sequence_mismatch(source_responses, target_responses)
    response_observation_match = response_observation_mismatch is None

    source_policy = selected_args(source, ("cache_write_policy",))
    target_policy = selected_args(target, ("cache_write_policy",))
    source_statuses = [row.get("status") for row in source.get("requests", []) if isinstance(row, dict)]
    target_statuses = [row.get("status") for row in target.get("requests", []) if isinstance(row, dict)]

    return {
        "schema": "trace_sim.hicache.workload_input_audit.v1",
        "source_label": args.source_label,
        "target_label": args.target_label,
        "source_report": str(args.source_report),
        "target_report": str(args.target_report),
        "source_request_count": len(source_requests),
        "target_request_count": len(target_requests),
        "source_phase_counts": phase_counts(source),
        "target_phase_counts": phase_counts(target),
        "request_shape_args_match": request_shape_args_match,
        "request_shape_args": {
            "source": source_shape_args,
            "target": target_shape_args,
            "differing_fields": sorted(key for key in REQUEST_SHAPE_ARG_KEYS if source_shape_args.get(key) != target_shape_args.get(key)),
        },
        "request_sequence_match": request_sequence_match,
        "first_request_sequence_mismatch": request_sequence_mismatch,
        "request_sequence_diff_samples": sequence_diff_samples(source_requests, target_requests, args.sample),
        "cache_write_policy_match": source_policy == target_policy,
        "cache_write_policy": {"source": source_policy.get("cache_write_policy"), "target": target_policy.get("cache_write_policy")},
        "response_observation_match": response_observation_match,
        "first_response_observation_mismatch": response_observation_mismatch,
        "response_observation_diff_samples": sequence_diff_samples(source_responses, target_responses, args.sample),
        "source_status_counts": {str(status): source_statuses.count(status) for status in sorted(set(source_statuses), key=str)},
        "target_status_counts": {str(status): target_statuses.count(status) for status in sorted(set(target_statuses), key=str)},
        "frontdoor_workload_ready": request_shape_args_match and request_sequence_match and all(status == "ok" for status in source_statuses + target_statuses),
        "note": (
            "Diagnostic only. frontdoor_workload_ready means the benchmark request shape and prompt identity fields match; "
            "it does not imply HiCache invariant state input streams are target-independent or cross-prediction-ready."
        ),
    }


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    report = build_report(args)
    if args.output:
        write_json(args.output, report)
        summary = {
            "schema": report["schema"],
            "output": str(args.output),
            "source_label": report["source_label"],
            "target_label": report["target_label"],
            "source_request_count": report["source_request_count"],
            "target_request_count": report["target_request_count"],
            "frontdoor_workload_ready": report["frontdoor_workload_ready"],
            "request_sequence_match": report["request_sequence_match"],
            "cache_write_policy_match": report["cache_write_policy_match"],
            "response_observation_match": report["response_observation_match"],
        }
        print(json.dumps(summary, ensure_ascii=False, indent=2))
    else:
        print(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
