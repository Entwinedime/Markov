"""Score complete HTTP windows after prediction, without feeding results back."""

from __future__ import annotations

from collections import Counter
import csv
import math
from pathlib import Path

from markov.artifacts.request import read_json
from markov.artifacts.writing import write_json
from markov.workflow.evaluation import normal_statistics


def metrics(rows: list[dict]) -> dict:
    scored = [row for row in rows if row["status"] == "scored"]
    complete = bool(rows) and len(scored) == len(rows)
    total_normal_sec = sum(row["normal_sec"] for row in scored)
    wape = (
        100 * sum(abs(row["predicted_sec"] - row["normal_sec"]) for row in scored) / total_normal_sec
        if scored
        else None
    )
    errors = sorted(row["ape_percent"] for row in scored)
    baseline_complete = complete and all(row["base_constant_ape_percent"] is not None for row in scored)
    return {
        "expected": len(rows),
        "predicted": sum(row["predicted_sec"] is not None for row in rows),
        "scored": len(scored),
        "wape_percent": wape if complete else None,
        "scored_subset_wape_percent": wape,
        "p90_ape_percent": errors[math.ceil(0.9 * len(errors)) - 1] if complete else None,
        "maximum_ape_percent": max(errors) if complete else None,
        "signed_error_percent": 100 * sum(row["predicted_sec"] - row["normal_sec"] for row in scored) / total_normal_sec
        if complete
        else None,
        "base_constant_wape_percent": sum(row["base_constant_ape_percent"] * row["normal_sec"] for row in scored)
        / total_normal_sec
        if baseline_complete
        else None,
    }


def grouped_metrics(rows: list[dict], field: str) -> dict[str, dict]:
    return {name: metrics([row for row in rows if row[field] == name]) for name in sorted({row[field] for row in rows})}


def write_report(output: Path, cases: list[dict], groups: dict[tuple[str, str], set[str]]) -> dict:
    predictions = read_json(output / "predictions.json")
    by_cell, group_status = {}, {}
    overhead = []
    for group in predictions["entries"]:
        key = group["base"], group["workload"]
        group_status[key] = group["status"]
        if "report" not in group:
            continue
        report = read_json(Path(group["report"]))
        if "cells" in report:
            overhead.append(
                {
                    "base": key[0],
                    "workload": key[1],
                    **{name: report[name] for name in ["workflow_sec", "source_prepare_sec", "process_peak_rss_bytes"]},
                }
            )
        for cell in report.get("cells", []):
            by_cell[(*key, cell["name"])] = cell

    normal = {
        (entry["configuration"], entry["workload"]): normal_statistics(entry["observations"])
        for entry in read_json(output / "measurements.json")["entries"]
    }
    rows = []
    all_cases = cases + [
        {"panel": "self", "base": base, "workload": workload, "target": base} for base, workload in groups
    ]
    for case in all_cases:
        base, workload, target = case["base"], case["workload"], case["target"]
        cell = by_cell.get((base, workload, target))
        actual = normal.get((target, workload))
        predicted = cell["formal_http_sec"] if cell else None
        observed = actual["mean_sec"] if actual else None
        status = cell["status"] if cell else group_status.get((base, workload), "not_predicted")
        row = {
            **case,
            "status": status,
            "predicted_sec": predicted,
            "normal_sec": observed,
            "normal_n": actual["n"] if actual else 0,
            "ape_percent": None,
            "signed_error_percent": None,
            "base_constant_ape_percent": None,
        }
        if predicted is not None:
            row["status"] = "normal_unavailable"
            if observed is not None:
                row.update(
                    status="scored",
                    ape_percent=100 * abs(predicted - observed) / observed,
                    signed_error_percent=100 * (predicted - observed) / observed,
                )
        baseline = normal.get((base, workload))
        if observed is not None and baseline is not None and baseline["mean_sec"] is not None:
            row["base_constant_ape_percent"] = 100 * abs(baseline["mean_sec"] - observed) / observed
        rows.append(row)

    cross = [row for row in rows if row["panel"] != "self"]
    summary = {
        "cross": metrics(cross),
        "self": metrics([row for row in rows if row["panel"] == "self"]),
        "panels": grouped_metrics(cross, "panel"),
        "bases": grouped_metrics(cross, "base"),
        "workloads": grouped_metrics(cross, "workload"),
        "targets": grouped_metrics(cross, "target"),
        "statuses": dict(Counter(row["status"] for row in cross)),
        "models": predictions["models"],
        "formal_prediction_overhead": overhead,
        "qualification": "Complete formal HTTP including client intervals, gaps, unassigned control and Prefill/Decode. Warmup excluded by declared boundary. Normal samples are shared by target configuration/workload; self is separate. No refitting from scores.",
        "normal_conditions": [{"configuration": key[0], "workload": key[1], **value} for key, value in normal.items()],
        "cells": rows,
    }
    write_json(output / "results.json", summary)
    with (output / "cells.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

    def number(value: float | None) -> str:
        return "—" if value is None else f"{value:.3f}"

    lines = [
        "# HiCache 跨配置预测结果",
        "",
        "计时取首个正式请求发出至最后一个正式响应结束，保留请求间隔、gap、未归属控制和 Prefill/Decode。",
        "",
        "正常实测按 target 配置与 workload 共享；同一实测被多个 base 引用，不代表多次独立采集。self 单独统计。",
        "",
        "| 面板 | 应有 cross | 已预测 | 已评分 | WAPE (%) | P90 APE (%) |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for panel, values in summary["panels"].items():
        lines.append(
            f"| {panel} | {values['expected']} | {values['predicted']} | {values['scored']} | {number(values['wape_percent'])} | {number(values['p90_ape_percent'])} |"
        )
    if summary["cross"]["scored"] != summary["cross"]["expected"]:
        lines += ["", "仍有缺项，完整矩阵精度尚不能给出；JSON 中另列已评分子集指标，失败没有记为零或从格数中删除。"]
    for label, field in (("Base", "bases"), ("Workload", "workloads"), ("Target", "targets")):
        lines += [
            "",
            f"| {label} | 应有 cross | 已评分 | WAPE (%) | P90 APE (%) | 沿用 base 的 WAPE (%) |",
            "| --- | ---: | ---: | ---: | ---: | ---: |",
        ]
        for name, values in summary[field].items():
            lines.append(
                f"| {name} | {values['expected']} | {values['scored']} | {number(values['wape_percent'])} | {number(values['p90_ape_percent'])} | {number(values['base_constant_wape_percent'])} |"
            )
    lines += [
        "",
        "误差最大的已评分 cross：",
        "",
        "| Workload | Base→Target | 预测 (秒) | 正常均值 (秒) | 样本数 | APE (%) |",
        "| --- | --- | ---: | ---: | ---: | ---: |",
    ]
    for row in sorted((row for row in cross if row["status"] == "scored"), key=lambda row: -row["ape_percent"])[:10]:
        lines.append(
            f"| {row['workload']} | {row['base']}→{row['target']} | {number(row['predicted_sec'])} | {number(row['normal_sec'])} | {row['normal_n']} | {number(row['ape_percent'])} |"
        )
    lines += [
        "",
        "完整逐格结果见 cells.csv 与 results.json。单次观察不提供重复波动证据；这些结果不单独证明 DAG 结构完整或受控效率达标。",
        "",
    ]
    (output / "results.md").write_text("\n".join(lines), encoding="utf-8")
    return summary
