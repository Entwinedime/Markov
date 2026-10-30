"""Prediction-only comparisons shared by user-facing applications."""

from __future__ import annotations

from typing import Any


def cost_query_domain(prediction: dict[str, Any]) -> str:
    """Classify operator-query evidence, not all DAG costs or confidence."""

    report = prediction.get("independent_operator_queries")
    if report is None:
        return "not_recorded"

    if not report["query_count"]:
        return "no_independent_operator_queries"
    if report["outside_range_queries"]:
        return "outside_measured_ranges"
    if report["unseen_combination_queries"]:
        return "within_ranges_unseen_combination"

    return "measured_operator_query_points"
