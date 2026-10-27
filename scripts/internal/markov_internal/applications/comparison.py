"""Prediction-only comparisons shared by user-facing applications."""

from __future__ import annotations

from typing import Any


def cost_query_domain(prediction: dict[str, Any]) -> str:
    """Classify operator-query evidence, not all DAG costs or confidence."""

    report = prediction.get("independent_operator_queries")
    if report is None:
        return "not_recorded"

    queries = report["rows"]
    if not queries:
        return "no_independent_operator_queries"
    if any(row["outside_range_queries"] for row in queries):
        return "outside_measured_ranges"
    if any(row["unseen_combination_queries"] for row in queries):
        return "within_ranges_unseen_combination"

    return "measured_operator_query_points"


def configuration_comparison(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Order predictions within one source/workload, without target observations.

    A self prediction is the only base-time reference here. Its absence remains
    missing: profiled source wall time is not a normal base measurement. Partial
    candidate sets have an order for inspection, but no asserted fastest target.
    """

    groups: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for row in rows:
        groups.setdefault((row["source_manifest"], row["workload_id"]), []).append(row)

    comparisons = []
    for (manifest, workload), group in groups.items():
        executed = [row for row in group if row["status"] == "EXECUTED"]
        ordered = sorted(executed, key=lambda row: row["http_e2e_us"])
        complete = len(executed) == len(group)
        base = next((row for row in executed if row["is_self"]), None)
        base_us = base["http_e2e_us"] if base is not None else None

        comparisons.append(
            {
                "source_manifest": manifest,
                "source_config_id": group[0]["source_config_id"],
                "workload_id": workload,
                "status": "predicted_order" if complete else "incomplete_candidates",
                "interpretation": "预测排序，不是已验证推荐，也不是统计置信区间。",
                "base_reference": {"kind": "self_prediction", "http_e2e_us": base_us} if base is not None else None,
                "predicted_fastest_targets": [
                    row["target_config"]
                    for row in ordered
                    if complete and row["http_e2e_us"] == ordered[0]["http_e2e_us"]
                ]
                if complete
                else None,
                "ordered_targets": [
                    {
                        "target_config": row["target_config"],
                        "http_e2e_us": row["http_e2e_us"],
                        "speedup_vs_base": base_us / row["http_e2e_us"] if base_us is not None else None,
                        "time_reduction_vs_base": 1 - row["http_e2e_us"] / base_us if base_us is not None else None,
                        "model_run_id": row["model_run_id"],
                    }
                    for row in ordered
                ],
                "unpredicted_targets": [row["target_config"] for row in group if row["status"] != "EXECUTED"],
            }
        )

    return comparisons
