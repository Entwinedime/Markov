"""Select a research method and fixed policies from completed development runs only."""

from __future__ import annotations

import argparse
from itertools import product
from pathlib import Path
import time
from typing import Any

from markov_internal.common.io import load_json, write_json
from markov_internal.common.paths import repo_relative_path, require_repo_path
from markov_internal.modeling.context import PredictionInputs
from markov_internal.modeling.invoke import invoke_prediction
from markov_internal.modeling.sglang_config import canonical_hicache, resolve_hicache_fields
from markov_internal.modeling.source_dag.profiles import cpu_service_metadata
from markov_internal.modeling.types import TargetHiCacheConfig
from markov_internal.applications.search.request import SearchRequest

CAPACITY_FIELDS = {"l1_capacity_pages", "l2_capacity_pages", "prefetch_capacity_limit_pages"}


def policy(fields: dict[str, Any]) -> dict[str, Any]:
    """Resource capacities are external; inactive timeout fields do not distinguish policies."""

    return {key: value for key, value in canonical_hicache(fields) if key not in CAPACITY_FIELDS}


def resolved_policy(fields: dict[str, Any], reference: TargetHiCacheConfig, name: str) -> TargetHiCacheConfig:
    page = fields["page_size"]
    resolved = {**reference.fields, **fields}
    for key in ("l1_capacity_pages", "l2_capacity_pages"):
        resolved[key] = reference.fields[key] * reference.fields["page_size"] // page

    return TargetHiCacheConfig(name, resolve_hicache_fields(resolved))


def add_scores(summary: dict[str, Any], scores: dict[tuple[Path, tuple], float]) -> None:
    """Update scores with the first successful prediction for each unchanged source/target pair."""

    for row in summary["prediction"]["cells"]:
        if row["status"] == "EXECUTED":
            key = require_repo_path(row["source_manifest"]), canonical_hicache(row["target_hicache"])
            scores.setdefault(key, row["http_e2e_us"] / 1e6)


def select_method(comparisons: list[tuple[dict[str, Any], dict[str, Any]]]) -> str:
    """Prefer local only for model improvement without extra charged queries on either task."""

    dominant = all(
        local["best_score_sec"] <= random["best_score_sec"]
        and local["charged_predictions"] <= random["charged_predictions"]
        for local, random in comparisons
    ) and any(local["best_score_sec"] < random["best_score_sec"] for local, random in comparisons)
    return "local" if dominant else "stratified_random"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument(
        "--output-dir", type=Path, required=True, help="A new result directory; retain failed attempts."
    )
    args = parser.parse_args()
    root = require_repo_path(args.root)
    protocol = load_json(root / "protocol.json")
    declarations = load_json(require_repo_path(protocol["development_comparison_plan"]))
    states = []
    execution = load_json(require_repo_path(declarations["requests"][0]))["execution"]
    peers = {}
    pool = {}
    scores = {}
    reports = []

    for path in declarations["requests"]:
        request = load_json(require_repo_path(path))
        state = load_json(require_repo_path(request["output_dir"]) / "search_summary.json")
        if state["status"] not in {"completed", "completed_with_gaps", "stopped"} or state["best_score_sec"] is None:
            raise ValueError(f"development prediction is incomplete: {path}")
        states.append(state)
        peers.setdefault((request["scenario"], request["reference_config"]), {})[request["search"]["algorithm"]] = state
        winner = min(
            (row for row in state["candidates"] if row["score_sec"] == state["best_score_sec"]),
            key=lambda row: row["space_index"],
        )
        fields = policy(winner["hicache"])
        pool[tuple(fields.items())] = fields
        for attempt in state["attempts"]:
            if attempt["report"] is not None:
                reports.append(attempt["report"])
                add_scores(load_json(require_repo_path(attempt["report"])), scores)

    comparisons = [(group["local"], group["stratified_random"]) for group in peers.values()]
    method = select_method(comparisons)

    for path in protocol["development_closure_requests"]:
        request = load_json(require_repo_path(path))
        state = load_json(require_repo_path(request["output_dir"]) / "search_summary.json")
        chosen = state["confirmation"]["screened_choice"]
        targets = {row["name"]: row["hicache"] for row in state["selected_candidates"]}
        targets[state["base_target"]["name"]] = state["base_target"]["hicache"]
        fields = policy(targets[chosen])
        pool[tuple(fields.items())] = fields

    references = {}
    for task in protocol["final_tasks"]:
        raw = load_json(require_repo_path(task["request"]))
        references[task["resource"]] = TargetHiCacheConfig.load(require_repo_path(raw["reference_config"]))
    original = policy(next(iter(references.values())).fields)
    pool[tuple(original.items())] = original
    axes = protocol["fixed_baseline_selection"]["pool_rule"]["fixed_selective_best_effort"]
    for page, threshold in product(axes["page_sizes"], axes["threshold_tokens"]):
        fields = {
            **original,
            "page_size": page,
            "write_policy": "write_through_selective",
            "prefetch_policy": "best_effort",
            "prefetch_threshold_pages": threshold // page,
        }
        pool[tuple(sorted(fields.items()))] = fields

    output = require_repo_path(args.output_dir)
    output.mkdir(parents=True, exist_ok=False)
    targets = {
        resource: [
            resolved_policy(fields, reference, f"{resource}_fixed_{i}") for i, fields in enumerate(pool.values())
        ]
        for resource, reference in references.items()
    }
    write_json(
        output / "selection_plan.json",
        {"method": method, "policies": list(pool.values()), "scope": protocol["fixed_baseline_selection"]},
    )
    sources = tuple(
        dict.fromkeys(
            require_repo_path(path) for state in states for path in state["prepared_inputs"]["source_manifests"]
        )
    )
    cpu = {
        require_repo_path(cpu_service_metadata(require_repo_path(path))["source_manifest"]): require_repo_path(path)
        for state in states
        for path in state["prepared_inputs"]["cpu_service_costs"]
    }
    executions = []
    reference_fields = {canonical_hicache(target.fields) for target in references.values()}
    prediction_started = time.monotonic()
    for index, source in enumerate(sources):
        missing = tuple(
            target
            for group in targets.values()
            for target in group
            if (source, canonical_hicache(target.fields)) not in scores
        )
        if not missing:
            continue
        # Score denominators first when the shared wall budget cannot cover the whole pool.
        missing = tuple(sorted(missing, key=lambda target: canonical_hicache(target.fields) not in reference_fields))

        remaining = protocol["fixed_baseline_selection"]["max_prediction_wall_sec"] - (
            time.monotonic() - prediction_started
        )
        if remaining <= 0:
            break
        inputs = PredictionInputs(
            (source,),
            require_repo_path(states[0]["prepared_inputs"]["cost_model"]),
            (cpu[source],),
            states[0]["prepared_inputs"]["resource_environment"],
        )
        outcome = invoke_prediction(
            inputs,
            missing,
            output / f"source_{index}",
            jobs=execution["model_run_jobs"],
            trace_threads=execution["trace_threads"],
            trace_file_threads=execution["trace_file_threads"],
            max_wall_sec=remaining,
        )
        charged = outcome.charged_predictions(len(missing))
        executions.append(
            {
                "source_manifest": str(repo_relative_path(source)),
                "report": str(repo_relative_path(outcome.report_path)),
                "return_code": outcome.return_code,
                "wall_seconds": outcome.wall_seconds,
                "submitted_predictions": len(missing),
                "charged_predictions": charged,
            }
        )
        write_json(output / "execution_summary.json", executions)
        if outcome.summary is None:
            raise RuntimeError(f"fixed-policy prediction produced no summary: {outcome.report_path}")
        reports.append(str(repo_relative_path(outcome.report_path)))
        add_scores(outcome.summary, scores)

    selected = {}
    pool_summary = []
    for resource, group in targets.items():
        candidates = []
        for target in group:
            keys = [(source, canonical_hicache(target.fields)) for source in sources]
            pool_summary.append(
                {
                    "resource": resource,
                    "target": target.label,
                    "hicache": target.fields,
                    "source_predictions_sec": [scores.get(key) for key in keys],
                    "sources": [str(repo_relative_path(source)) for source in sources],
                }
            )
            if not all(key in scores for key in keys):
                continue
            ratios = [
                scores[key] / scores[source, canonical_hicache(references[resource].fields)]
                for source, key in zip(sources, keys)
            ]
            mean = sum(ratios) / len(ratios)
            pool_summary[-1]["mean_relative_http"] = mean
            candidates.append((mean, target))
        fixed = min(candidates, key=lambda item: item[0])[1]
        best_effort = min(
            (
                item
                for item in candidates
                if item[1].fields["write_policy"] == "write_through_selective"
                and item[1].fields["prefetch_policy"] == "best_effort"
            ),
            key=lambda item: item[0],
        )[1]
        selected[resource] = {}
        for kind, target in (("development_fixed", fixed), ("best_effort", best_effort)):
            path = output / f"{resource}_{kind}.json"
            write_json(path, {"name": target.label, "hicache": target.fields})
            selected[resource][kind] = str(repo_relative_path(path))

    result = {
        "method": method,
        "selected": selected,
        "prediction_reports": reports,
        "executions": executions,
        "candidates": pool_summary,
        "scope": "Finite development prediction pool; unknown points omitted from selection, never assigned zero. Not measured or global best; no final measurements used.",
    }
    write_json(output / "fixed_baselines.json", result)
    protocol["fixed_baseline_selection"].update(
        status="selected", report=str(repo_relative_path(output / "fixed_baselines.json"))
    )
    protocol["final_method_selection"].update(
        status="selected",
        method=method,
        rule="Local must be no worse in model minimum and charged attempts on both development tasks, with a strict model improvement on at least one; otherwise use stratified random. This does not establish real superiority.",
    )
    for task in protocol["final_tasks"]:
        path = require_repo_path(task["request"])
        raw = load_json(path)
        raw["search"]["algorithm"] = method
        request = SearchRequest.load(path)
        fixed = TargetHiCacheConfig.load(require_repo_path(selected[task["resource"]]["development_fixed"]))
        raw["search"]["initial_indices"] = [request.space.locate(fixed.fields)]
        write_json(path, raw)
        task["algorithm_status"] = "selected_from_development"
    write_json(root / "protocol.json", protocol)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
