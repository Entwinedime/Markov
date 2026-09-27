"""Full client E2E scoring against bench measurements, without cost exclusions."""

from __future__ import annotations

import math
from statistics import mean

from ...modeling.workload import WorkloadWindow


def score_http(run: dict, source: tuple[WorkloadWindow | None, ...], target: tuple[WorkloadWindow | None, ...]) -> dict:
    """Compare duration means; retain real sample windows, never average trace coordinates."""

    http = run.get("http_client", {})
    result = {
        "ready": False,
        "metric": "full_http_formal_window",
        "exclusions": [],
        "http_status": http.get("status", "missing"),
        "reason": "",
    }
    if http.get("status") != "connected":
        return {**result, "reason": "prediction_http_not_connected"}
    for role, windows in (("source", source), ("target", target)):
        if not windows or any(
            window is None or window.source != "workload_report.formal_window" or window.actual_e2e_ns <= 0
            for window in windows
        ):
            return {**result, "reason": f"{role}_formal_http_window_missing"}
    predicted = http.get("e2e_us")
    if (
        not isinstance(predicted, (int, float))
        or isinstance(predicted, bool)
        or not math.isfinite(predicted)
        or predicted <= 0
    ):
        return {**result, "reason": "prediction_http_duration_invalid"}
    target_us = [window.actual_e2e_ns / 1000 for window in target]
    source_us = [window.actual_e2e_ns / 1000 for window in source]
    actual, base = mean(target_us), mean(source_us)
    absolute = abs(predicted - actual)
    return {
        **result,
        "ready": True,
        "predicted_us": predicted,
        "target_us": actual,
        "base_us": base,
        "aggregation": "arithmetic_mean",
        "source_reports": [str(window.report_path) for window in source],
        "target_reports": [str(window.report_path) for window in target],
        "source_sample_count": len(source),
        "target_sample_count": len(target),
        "source_range_us": [min(source_us), max(source_us)],
        "target_range_us": [min(target_us), max(target_us)],
        "absolute_error_us": absolute,
        "signed_error_us": predicted - actual,
        "ape": absolute / actual,
        "base_absolute_error_us": abs(base - actual),
        "base_ape": abs(base - actual) / actual,
        "target_delta_us": actual - base,
        "predicted_delta_us": predicted - base,
    }


def http_metrics(rows: list[dict]) -> dict:
    values = [row["full_e2e"] for row in rows if row.get("full_e2e", {}).get("ready") is True]
    total = sum(item["target_us"] for item in values)
    absolute = sum(item["absolute_error_us"] for item in values)
    base_error = sum(item["base_absolute_error_us"] for item in values)
    apes = sorted(item["ape"] for item in values)
    cross = [row["full_e2e"] for row in rows if not row["is_self"] and row.get("full_e2e", {}).get("ready") is True]
    cross_error = sum(item["absolute_error_us"] for item in cross)
    cross_base_error = sum(item["base_absolute_error_us"] for item in cross)
    return {
        "metric": "full_http_formal_window",
        "exclusions": [],
        "expected_cell_count": len(rows),
        "cell_count": len(values),
        "coverage_complete": bool(rows) and len(values) == len(rows),
        "missing_cells": [
            {"model_run_id": row["model_run_id"], "reason": row.get("full_e2e", {}).get("reason", "http_score_missing")}
            for row in rows
            if row.get("full_e2e", {}).get("ready") is not True
        ],
        "wape": absolute / total if total else None,
        "p90_ape": apes[math.ceil(0.9 * len(apes)) - 1] if apes else None,
        "absolute_error_us": absolute,
        "target_total_us": total,
        "base_wall_wape": base_error / total if total else None,
        "base_wall_absolute_error_us": base_error,
        "cross_cell_count": len(cross),
        "better_than_base_wall": cross_error < cross_base_error if cross else None,
        "over_5pct": [
            {
                "model_run_id": row["model_run_id"],
                "source_config_id": row.get("source_config_id"),
                "target_config": row.get("target_config"),
                "workload_id": row.get("workload_id"),
                "ape": row["full_e2e"]["ape"],
                "signed_error_seconds": row["full_e2e"]["signed_error_us"] / 1e6,
            }
            for row in rows
            if row.get("full_e2e", {}).get("ready") and row["full_e2e"]["ape"] > 0.05
        ],
    }


def http_gates(metrics: dict) -> dict[str, bool]:
    """Apply acceptance thresholds to the already computed HTTP summary."""
    covered = metrics["coverage_complete"]
    return {
        "full_e2e": covered and metrics["wape"] <= 0.03 and metrics["p90_ape"] <= 0.05,
        "base_wall": covered and (metrics["cross_cell_count"] == 0 or metrics["better_than_base_wall"] is True),
    }
