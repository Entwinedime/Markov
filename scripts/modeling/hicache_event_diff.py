#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path
from typing import Any


def as_number(value: Any) -> float:
    if value is None or value == "" or value == "null":
        return 0.0
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0.0


def as_text(args: dict[str, Any], key: str, fallback: str = "") -> str:
    value = args.get(key, fallback)
    if value is None or value == "null":
        return ""
    return str(value)


def should_keep_event(event: dict[str, Any], lane: str) -> bool:
    if event.get("ph") != "X":
        return False
    args = event.get("args") or {}
    if args.get("domain") != "cache_io":
        return False
    name = str(event.get("name", ""))
    if not (name.startswith("node_") or name.startswith("HiCache::")):
        return False
    try:
        pid = int(event.get("pid", 0))
    except (TypeError, ValueError):
        return False
    if lane == "original":
        return pid >= 100000
    if lane == "simulated":
        return pid < 100000
    return True


def event_signature(
    event: dict[str, Any],
    *,
    include_page_size: bool,
) -> tuple[str, ...]:
    args = event.get("args") or {}
    name = str(event.get("name", ""))
    if name.startswith("node_"):
        name = name[5:]
    signature = [
        as_text(args, "cache_io.event_kind") or as_text(args, "event_kind"),
        as_text(args, "cache_io.src") or as_text(args, "tier_src"),
        as_text(args, "cache_io.dst") or as_text(args, "tier_dst"),
        as_text(args, "direction"),
        name,
        as_text(args, "python_class"),
        as_text(args, "python_method"),
        as_text(args, "status"),
    ]
    if include_page_size:
        signature.append(as_text(args, "page_size"))
    return tuple(signature)


def load_rows(
    path: Path,
    *,
    include_page_size: bool,
    lane: str,
) -> dict[tuple[str, ...], dict[str, float]]:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)

    rows: dict[tuple[str, ...], dict[str, float]] = defaultdict(
        lambda: {
            "count": 0.0,
            "pages": 0.0,
            "bytes": 0.0,
            "estimated_us": 0.0,
            "ori_us": 0.0,
        }
    )
    for event in data.get("traceEvents", []):
        if not should_keep_event(event, lane):
            continue
        args = event.get("args") or {}
        if args.get("domain") != "cache_io":
            continue
        key = event_signature(event, include_page_size=include_page_size)
        row = rows[key]
        row["count"] += 1
        row["pages"] += as_number(args.get("cache_io.pages")) or as_number(
            args.get("num_pages")
        )
        row["bytes"] += as_number(args.get("cache_io.bytes")) or as_number(
            args.get("bytes")
        )
        row["estimated_us"] += as_number(args.get("cache_io.estimated_time"))
        row["ori_us"] += as_number(args.get("ori_time")) or as_number(event.get("dur"))
    return rows


def write_rows(
    path: Path,
    rows: dict[tuple[str, ...], dict[str, float]],
    *,
    include_page_size: bool,
) -> None:
    headers = [
        "event_kind",
        "src",
        "dst",
        "direction",
        "name",
        "class",
        "method",
        "status",
    ]
    if include_page_size:
        headers.append("page_size")
    headers.extend(["count", "pages", "bytes", "estimated_us", "ori_us"])

    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(headers)
        for key, row in sorted(rows.items()):
            writer.writerow(
                [
                    *key,
                    int(row["count"]),
                    int(row["pages"]),
                    int(row["bytes"]),
                    int(row["estimated_us"]),
                    int(row["ori_us"]),
                ]
            )


def write_diff(
    path: Path,
    predicted: dict[tuple[str, ...], dict[str, float]],
    actual: dict[tuple[str, ...], dict[str, float]],
    *,
    include_page_size: bool,
) -> list[dict[str, Any]]:
    headers = [
        "event_kind",
        "src",
        "dst",
        "direction",
        "name",
        "class",
        "method",
        "status",
    ]
    if include_page_size:
        headers.append("page_size")
    metric_headers = [
        "pred_count",
        "actual_count",
        "delta_count",
        "pred_pages",
        "actual_pages",
        "delta_pages",
        "pred_bytes",
        "actual_bytes",
        "delta_bytes",
        "pred_estimated_us",
        "actual_estimated_us",
        "delta_estimated_us",
        "pred_ori_us",
        "actual_ori_us",
        "delta_ori_us",
    ]
    top_rows: list[dict[str, Any]] = []

    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow([*headers, *metric_headers])
        for key in sorted(set(predicted) | set(actual)):
            pred = predicted.get(
                key,
                {
                    "count": 0.0,
                    "pages": 0.0,
                    "bytes": 0.0,
                    "estimated_us": 0.0,
                    "ori_us": 0.0,
                },
            )
            act = actual.get(
                key,
                {
                    "count": 0.0,
                    "pages": 0.0,
                    "bytes": 0.0,
                    "estimated_us": 0.0,
                    "ori_us": 0.0,
                },
            )
            values = {
                "pred_count": pred["count"],
                "actual_count": act["count"],
                "delta_count": pred["count"] - act["count"],
                "pred_pages": pred["pages"],
                "actual_pages": act["pages"],
                "delta_pages": pred["pages"] - act["pages"],
                "pred_bytes": pred["bytes"],
                "actual_bytes": act["bytes"],
                "delta_bytes": pred["bytes"] - act["bytes"],
                "pred_estimated_us": pred["estimated_us"],
                "actual_estimated_us": act["estimated_us"],
                "delta_estimated_us": pred["estimated_us"] - act["estimated_us"],
                "pred_ori_us": pred["ori_us"],
                "actual_ori_us": act["ori_us"],
                "delta_ori_us": pred["ori_us"] - act["ori_us"],
            }
            writer.writerow([*key, *[int(values[name]) for name in metric_headers]])
            score = (
                abs(values["delta_count"])
                + abs(values["delta_pages"])
                + abs(values["delta_estimated_us"]) / 1000.0
            )
            if score:
                top_rows.append(
                    {
                        "score": score,
                        "signature": dict(zip(headers, key)),
                        **{name: int(value) for name, value in values.items()},
                    }
                )

    top_rows.sort(key=lambda row: row["score"], reverse=True)
    return top_rows


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare cache_io events from two TraceGraph full-output traces."
    )
    parser.add_argument("--predicted", required=True, type=Path)
    parser.add_argument("--actual", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument(
        "--include-page-size",
        action="store_true",
        help="Include event page_size in the aggregation key.",
    )
    parser.add_argument(
        "--lane",
        choices=("original", "simulated", "all"),
        default="original",
        help="Which full-output lane to compare. original keeps pid >= 100000.",
    )
    parser.add_argument("--top", type=int, default=25)
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    predicted = load_rows(
        args.predicted, include_page_size=args.include_page_size, lane=args.lane
    )
    actual = load_rows(args.actual, include_page_size=args.include_page_size, lane=args.lane)

    suffix = "by_page_size" if args.include_page_size else "ignore_page_size"
    write_rows(
        args.output_dir / f"predicted_cache_io_events.{suffix}.csv",
        predicted,
        include_page_size=args.include_page_size,
    )
    write_rows(
        args.output_dir / f"actual_cache_io_events.{suffix}.csv",
        actual,
        include_page_size=args.include_page_size,
    )
    top_rows = write_diff(
        args.output_dir / f"event_diff.{suffix}.csv",
        predicted,
        actual,
        include_page_size=args.include_page_size,
    )
    summary = {
        "predicted_trace": str(args.predicted),
        "actual_trace": str(args.actual),
        "include_page_size": args.include_page_size,
        "lane": args.lane,
        "predicted_signatures": len(predicted),
        "actual_signatures": len(actual),
        "top_diffs": top_rows[: args.top],
    }
    with (args.output_dir / f"event_diff_summary.{suffix}.json").open(
        "w", encoding="utf-8"
    ) as f:
        json.dump(summary, f, indent=2, ensure_ascii=False)
        f.write("\n")
    for row in top_rows[: args.top]:
        print(json.dumps(row, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
