"""Full client E2E scoring against bench measurements, without cost exclusions."""

from __future__ import annotations

import math
from statistics import mean

from markov_internal.modeling.workload import WorkloadWindow


def score_cell(
    prediction: dict,
    run: dict,
    target_run_id: str,
    *,
    source_windows: tuple[WorkloadWindow | None, ...] = (),
    target_windows: tuple[WorkloadWindow | None, ...] = (),
) -> dict:
    """Score completed predictions without extracting component or target-DAG costs."""
    return {
        **{key: value for key, value in prediction.items() if key not in {"shape", "target_hicache"}},
        "target_run_id": target_run_id,
        "target_observation_used": True,
        "full_e2e": score_http(run, source_windows, target_windows),
    }


def score_metrics(rows: list[dict]) -> dict:
    """HTTP-only acceptance; oracle diagnostics do not change this result."""
    http = http_metrics(rows)
    gates = http_gates(http)
    return {
        "status": "PASS" if all(gates.values()) else "MODEL_LIMITATION",
        "acceptance_scope": "full_http_formal_window",
        "cell_count": len(rows),
        "gates": gates,
        "full_e2e": http,
    }


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
    signed = sum(item["signed_error_us"] for item in values)
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
        "max_ape": apes[-1] if apes else None,
        "signed_error_us": signed,
        "weighted_signed_error": signed / total if total else None,
        "mean_signed_error_us": signed / len(values) if values else None,
        "bias_convention": "prediction minus measurement; positive means overprediction",
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


def configuration_selection(rows: list[dict]) -> list[dict]:
    """Evaluate candidate choice only after independent HTTP scoring.

    Disjoint observed sample ranges define distinguishable pairs. Overlap is
    insufficient separation, not equivalence or a confidence interval. Counts
    are per source prediction: different bases can reuse the same measurements.
    This report never changes models, readiness or the HTTP accuracy gates.
    """

    groups: dict[tuple[str, str], list[dict]] = {}
    for row in rows:
        groups.setdefault((row["source_manifest"], row["workload_id"]), []).append(row)

    results = []
    for (manifest, workload), candidates in groups.items():
        result = {
            "source_manifest": manifest,
            "source_config_id": candidates[0]["source_config_id"],
            "workload_id": workload,
            "candidate_count": len(candidates),
            "scope": "Declared candidates only; repeated base comparisons do not create independent normal samples.",
            "separation_rule": "Disjoint observed sample ranges; overlapping ranges are inconclusive, not confidence intervals.",
        }
        if any(not row["full_e2e"]["ready"] for row in candidates):
            results.append({**result, "status": "NOT_READY", "predicted_best_targets": None, "choice_losses": None})
            continue

        costs = {row["target_config"]: row["full_e2e"] for row in candidates}
        predicted_best = min(cost["predicted_us"] for cost in costs.values())
        measured_best = min(cost["target_us"] for cost in costs.values())
        choices = [name for name, cost in costs.items() if cost["predicted_us"] == predicted_best]
        losses = [
            {"target_config": name, "relative_loss": (costs[name]["target_us"] - measured_best) / measured_best}
            for name in choices
        ]

        correct = inversions = ties = 0
        pairs = []
        overlaps = []
        names = sorted(costs)
        for index, first in enumerate(names):
            for second in names[index + 1 :]:
                a, b = costs[first], costs[second]
                if a["target_range_us"][1] < b["target_range_us"][0]:
                    faster, slower = first, second
                elif b["target_range_us"][1] < a["target_range_us"][0]:
                    faster, slower = second, first
                else:
                    overlaps.append({"first": first, "second": second})
                    continue

                difference_us = costs[faster]["predicted_us"] - costs[slower]["predicted_us"]
                if difference_us < 0:
                    correct += 1
                    order = "correct"
                elif difference_us > 0:
                    inversions += 1
                    order = "inverted"
                else:
                    ties += 1
                    order = "tied"

                pairs.append(
                    {
                        "measured_faster": faster,
                        "measured_slower": slower,
                        "prediction_order": order,
                    }
                )

        if not pairs:
            status = "INSUFFICIENT_SEPARATION"
        elif inversions:
            status = "INCORRECT_ORDER"
        elif ties:
            status = "UNRESOLVED_ORDER"
        else:
            status = "SUPPORTED_ON_MEASURED_PAIRS"
        results.append(
            {
                **result,
                "status": status,
                "predicted_best_targets": choices,
                "measured_best_targets": [name for name, cost in costs.items() if cost["target_us"] == measured_best],
                "choice_losses": losses,
                "correctly_ordered_pairs": correct,
                "inverted_pairs": inversions,
                "predicted_ties_on_distinguishable_pairs": ties,
                "overlapping_sample_ranges": len(overlaps),
                "inconclusive_pairs": overlaps,
                "distinguishable_pairs": pairs,
            }
        )

    return results


def http_gates(metrics: dict) -> dict[str, bool]:
    """Apply acceptance thresholds to the already computed HTTP summary."""
    covered = metrics["coverage_complete"]
    return {
        "full_e2e": covered and metrics["wape"] <= 0.03 and metrics["p90_ape"] <= 0.05,
        "base_wall": covered and (metrics["cross_cell_count"] == 0 or metrics["better_than_base_wall"] is True),
    }
