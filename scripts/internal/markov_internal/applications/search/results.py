"""Read explicit prediction attempts and rank only complete workload combinations."""

from __future__ import annotations

from collections import Counter
import math
from typing import Any

from markov_internal.applications.comparison import cost_query_domain
from markov_internal.common.io import load_json
from markov_internal.common.paths import require_repo_path
from markov_internal.applications.search.request import SearchRequest


def prediction_rows(state: dict[str, Any]) -> dict[tuple[str, str], dict[str, Any]]:
    """Reuse successful results within this comparison; preserve failures by reference."""

    rows = {}
    for attempt in state["attempts"]:
        if attempt["report"] is None:
            continue

        summary = load_json(require_repo_path(attempt["report"]))
        for row in summary["prediction"]["cells"]:
            key = row["target_config"], row["workload_id"]
            if key not in rows or rows[key]["status"] != "EXECUTED":
                rows[key] = row

    return rows


def update_results(request: SearchRequest, state: dict[str, Any]) -> None:
    """Update compact results without opening target evidence or changing costs."""

    rows = prediction_rows(state)
    candidates = []
    base = state.get("base_target")

    def result(target: dict[str, Any]) -> dict[str, Any]:
        values = {}
        for workload in request.weights:
            row = rows.get((target["name"], workload))
            values[workload] = (
                {
                    "status": row["status"],
                    "http_e2e_sec": row["http_e2e_us"] / 1_000_000 if row["http_e2e_us"] is not None else None,
                    "cost_coverage": row["cost_coverage"],
                    "operator_query_domain": cost_query_domain(row),
                    "approximations": row["approximations"],
                    "blockers": row["blockers"],
                    "missing_costs": row["missing_costs"],
                    "structure_gaps": row["structure_gaps"],
                    "failure_log": row["failure_log"],
                }
                if row is not None
                else {"status": "NOT_EVALUATED", "http_e2e_sec": None}
            )
        complete = all(row["status"] == "EXECUTED" for row in values.values())
        score = (
            math.fsum(request.weights[name] * row["http_e2e_sec"] for name, row in values.items()) if complete else None
        )
        return {**target, "workloads": values, "score_sec": score}

    for selected in state["selected_candidates"]:
        candidates.append(result(selected))

    baseline = result(base) if base is not None else None
    reference = result(state["reference_target"]) if "reference_target" in state else baseline
    ranked = sorted((row for row in candidates if row["score_sec"] is not None), key=lambda row: row["score_sec"])
    baseline_score = baseline["score_sec"] if baseline is not None else None
    for row in ranked:
        row["time_reduction_vs_predicted_base"] = (
            1 - row["score_sec"] / baseline_score if baseline_score is not None else None
        )
        row["time_reduction_vs_predicted_reference"] = (
            1 - row["score_sec"] / reference["score_sec"]
            if reference is not None and reference["score_sec"] is not None
            else None
        )

    best = ranked[0]["score_sec"] if ranked else None
    covered = {
        request.space.branch_index(row["space_index"])
        for row in candidates
        if any(rows.get((row["name"], name), {}).get("attempted", False) for name in request.weights)
    }
    state.update(
        candidates=candidates,
        baseline=baseline,
        same_resource_reference=reference,
        ranked_candidates=[row["name"] for row in ranked],
        best_candidates=[row["name"] for row in ranked if row["score_sec"] == best],
        best_score_sec=best,
        found_predicted_improvement=best < baseline_score if best is not None and baseline_score is not None else None,
        successful_configurations=len(ranked),
        incomplete_configurations=len(candidates) - len(ranked),
        unselected_configurations=request.space.count - len(candidates) - len(state["rejected_candidates"]),
        configurations_without_complete_prediction=request.space.count - len(ranked),
        missing_cost_configurations=sum(
            any(value.get("missing_costs") for value in row["workloads"].values()) for row in candidates
        ),
        structure_gap_configurations=sum(
            any(value.get("structure_gaps") for value in row["workloads"].values()) for row in candidates
        ),
        fully_evaluated_space=len(ranked) == request.space.count and request.space.count > 0,
        global_model_optimum_proven=len(ranked) == request.space.count and request.space.count > 0,
        covered_branches=sorted(covered),
        uncovered_branches=[i for i in range(len(request.space.branches)) if i not in covered],
        cost_coverage_counts=dict(
            Counter(row["cost_coverage"] for row in rows.values() if row["status"] == "EXECUTED")
        ),
        interpretation="Model ranking only; unknown points are not slower, partial costs and retained base waits do not establish real speedup.",
    )
