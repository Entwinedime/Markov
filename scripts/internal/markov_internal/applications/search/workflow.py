"""Prepare once, choose a bounded batch, and call the unified predictor."""

from __future__ import annotations

from dataclasses import replace
from itertools import chain
import time
from typing import Any

from markov_internal.applications.search.algorithms import candidate_indices
from markov_internal.applications.search.request import SearchRequest
from markov_internal.applications.search.results import prediction_rows, update_results
from markov_internal.common.io import load_json, write_json, write_json_atomic
from markov_internal.common.paths import repo_relative_path, require_repo_path
from markov_internal.modeling.invoke import PredictionClient
from markov_internal.modeling.sglang_config import hicache_server_flags
from markov_internal.modeling.source_dag.profiles import (
    cpu_service_metadata,
    parse_json_object_or_empty,
    parse_server_command_tokens,
)
from markov_internal.common.commands import command_tokens
from markov_internal.modeling.types import TargetHiCacheConfig
from markov_internal.preparation.base import PreparedInputs, prepare_inputs
from markov_internal.preparation.scenario import ScenarioRequest


def search_plan(request: SearchRequest) -> dict[str, Any]:
    """No graph, training, services or normal measurements are started here."""

    space = request.space
    return {
        "configurations": space.count,
        "statically_invalid_capacity_combinations": space.invalid_capacity_combinations,
        "branches": [branch.description for branch in space.branches],
        "capacity_unit": "targets use pages; *_tokens axes convert per page size; prefetch capacity is derived, not searched",
        "algorithm": request.algorithm,
        "seed": request.seed,
        "initial_indices": list(request.initial_indices),
        "objective": {"metric": "full_http_e2e_sec", "weights": request.weights, "exclusions": []},
        "max_predictions": request.max_predictions,
        "max_wall_sec": request.max_wall_sec,
        "self_reference": "one prediction per workload; reused if the same execution point is in the space",
        "input_preparation": {
            "missing_base_workloads": sorted(set(request.weights) & set(request.scenario.missing_base_workloads)),
            "cpu_pairing_budget_declared": request.scenario.cpu_service.capture_budget is not None,
            "cost_basis": str(repo_relative_path(request.scenario.cost_model))
            if request.scenario.cost_model
            else "offline preparation required",
            "runtime_cost_coverage": "unknown until actual target work is expanded",
        },
        "real_feasibility": "not established by configuration parsing or prediction",
        "confirmation_enabled": request.confirmation["enabled"],
        "reference_config": request.reference.label if request.reference else "source Base",
    }


def _prepare(request: SearchRequest, state: dict[str, Any]) -> tuple[PreparedInputs, float]:
    """Materialize only selected workloads; keep capture recovery in preparation."""

    raw = load_json(request.scenario.path)
    raw.pop("target_configs", None)
    raw.update(
        base_config=request.scenario.base_config,
        workload_ids=list(request.weights),
        output_dir=str(repo_relative_path(request.output_dir / "preparation")),
    )
    selected = [source for source in request.scenario.sources if source.input_id in request.weights]
    raw["base_manifests"] = [str(repo_relative_path(source.manifest_path)) for source in selected]
    if "prepared_inputs" in state:
        saved = state["prepared_inputs"]
        raw.update(
            base_manifests=saved["source_manifests"],
            cpu_service_costs=saved["cpu_service_costs"],
            cost_model=saved["cost_model"],
        )
    selected_manifests = {require_repo_path(value).resolve() for value in raw["base_manifests"]}
    costs = [
        value
        for value in raw.get("cpu_service_costs", [])
        if require_repo_path(cpu_service_metadata(require_repo_path(value))["source_manifest"]).resolve()
        in selected_manifests
    ]
    raw["cpu_service_costs"] = costs
    covered = {
        require_repo_path(cpu_service_metadata(require_repo_path(value))["source_manifest"]).resolve()
        for value in costs
    }
    if "cpu_service_pairs" in raw:
        raw["cpu_service_pairs"] = [
            pair
            for pair in raw["cpu_service_pairs"]
            if require_repo_path(pair["profile_manifest"]).resolve() in selected_manifests - covered
        ]

    path = request.output_dir / "inputs" / "scenario.json"
    write_json(path, raw)
    started = time.monotonic()
    prepared = prepare_inputs(ScenarioRequest.load(path))
    return prepared, time.monotonic() - started


def run_search(request: SearchRequest, *, dry_run: bool = False, resume: bool = False) -> dict[str, Any]:
    """Local search reuses the batch predictor and cumulative accounting, not a second simulator."""

    with PredictionClient(
        request.output_dir / "prediction_sessions",
        jobs=request.execution["model_run_jobs"],
        trace_threads=request.execution["trace_threads"],
        trace_file_threads=request.execution["trace_file_threads"],
        diagnostics=request.execution["diagnostics"],
        experiment_reports=request.execution.get("experiment_reports", False),
        dry_run=dry_run,
    ) as predictor:
        state = _search_rounds(request, predictor, dry_run=dry_run, resume=resume)
    if not dry_run:
        close_seconds = predictor.last_close_wall_seconds
        state["prediction_session_close_wall_seconds"] = close_seconds
        for field in ("search_wall_seconds", "total_wall_seconds", "invocation_wall_seconds"):
            state[field] += close_seconds
        write_json_atomic(request.output_dir / "search_summary.json", state)
    return state


def _search_rounds(
    request: SearchRequest, predictor: PredictionClient, *, dry_run: bool, resume: bool
) -> dict[str, Any]:
    if dry_run or request.algorithm not in {"local", "coordinate"}:
        return _run_batch(request, predictor, dry_run=dry_run, resume=resume)

    charged = 0
    unchanged = 0
    previous_best = None
    if resume:
        previous = load_json(request.output_dir / "search_summary.json")
        charged, previous_best = previous["charged_predictions"], previous["best_score_sec"]
    batch = max(32, len(request.space.branches)) * len(request.weights)
    while True:
        limit = min(request.max_predictions, charged + batch)
        state = _run_batch(replace(request, max_predictions=limit), predictor, resume=resume, explore=unchanged >= 2)
        count = state["charged_predictions"]
        if count <= charged or count >= request.max_predictions or state["stop_reason"] != "prediction_budget":
            state["plan"]["max_predictions"] = request.max_predictions
            write_json_atomic(request.output_dir / "search_summary.json", state)
            return state
        unchanged = unchanged + 1 if state["best_score_sec"] == previous_best else 0
        if unchanged > 2:
            unchanged = 0
        previous_best, charged, resume = state["best_score_sec"], count, True


def _run_batch(
    request: SearchRequest,
    predictor: PredictionClient,
    *,
    dry_run: bool = False,
    resume: bool = False,
    explore: bool = False,
) -> dict[str, Any]:
    """A comparison uses unchanged inputs/code/costs. Resume explicitly affirms that.

    Successful cells are reused only within this output. Interrupted/failed
    attempts remain referenced and retries consume the cumulative budget.
    No fingerprint system tries to prove that files were not edited in place.
    """

    started = time.monotonic()
    output = request.output_dir
    plan = search_plan(request)
    if dry_run:
        write_json(output / "search_plan.json", plan)
        return {"status": "planned", **plan}

    report = output / "search_summary.json"
    if report.exists():
        if not resume:
            raise ValueError("search output already exists; use --resume only with unchanged inputs, code and costs")
        state = load_json(report)
        if state["basis"] != request.basis:
            raise ValueError("comparison inputs changed; use a new output instead of mixing predictions")
        if any(attempt["status"] == "running" for attempt in state["attempts"]):
            raise ValueError("previous invocation is unsettled; reconcile its process/container before resuming")
    else:
        state = {
            "basis": request.basis,
            "attempts": [],
            "selected_candidates": [],
            "rejected_candidates": [],
            "preparation_wall_seconds": 0.0,
            "search_wall_seconds": 0.0,
            "total_wall_seconds": 0.0,
        }

    previous_wall = state["search_wall_seconds"]
    previous_total = state["total_wall_seconds"]
    search_started = None
    state.update(status="preparing", stop_reason=None, plan=plan)
    update_results(request, state)
    write_json_atomic(report, state)
    try:
        if not request.space.count:
            state.update(status="completed", stop_reason="no_candidates")
            return state
        if request.max_predictions < len(request.weights):
            state.update(status="stopped", stop_reason="prediction_budget")
            return state
        if previous_wall >= request.max_wall_sec:
            state.update(status="stopped", stop_reason="wall_budget")
            return state

        prepared, preparation_wall = _prepare(request, state)
        state["preparation_wall_seconds"] += preparation_wall
        inputs = prepared.prediction_inputs
        if inputs is None:
            state.update(status="preparation_incomplete", stop_reason=prepared.summary["status"])
            return state

        state["prepared_inputs"] = {
            "source_manifests": [str(repo_relative_path(path)) for path in inputs.source_manifests],
            "cpu_service_costs": [str(repo_relative_path(path)) for path in inputs.cpu_service_costs],
            "cost_model": str(repo_relative_path(inputs.cost_model)),
            "resource_environment": inputs.resource_environment,
        }
        source = prepared.scenario.sources[0]
        base = TargetHiCacheConfig.from_raw({"name": source.config_id, "hicache": source.hicache_config})
        state["base_target"] = {"name": base.label, "hicache": base.fields}
        base_index = request.space.locate(base.fields)
        source_flags = parse_server_command_tokens(command_tokens(load_json(source.config_path)["server"]["command"]))
        extra = parse_json_object_or_empty(source_flags.get("hicache_storage_backend_extra_config"))
        search_started = time.monotonic()
        update_results(request, state)
        existing = prediction_rows(state)
        budget = request.max_predictions - sum(attempt["charged_predictions"] for attempt in state["attempts"])
        scheduled: dict[str, list[TargetHiCacheConfig]] = {name: [] for name in request.weights}

        def schedule(target: TargetHiCacheConfig) -> bool:
            nonlocal budget
            workloads = [
                name for name in request.weights if existing.get((target.label, name), {}).get("status") != "EXECUTED"
            ]
            if len(workloads) > budget:
                return False

            for name in workloads:
                if target.label not in {item.label for item in scheduled[name]}:
                    scheduled[name].append(target)
                    budget -= 1
            return True

        def remember(index: int, target: TargetHiCacheConfig) -> None:
            if index not in {row["space_index"] for row in state["selected_candidates"]}:
                state["selected_candidates"].append(
                    {
                        "space_index": index,
                        "name": target.label,
                        "hicache": target.fields,
                        "server_flags": {
                            "--" + key.replace("_", "-"): value
                            for key, value in hicache_server_flags(target.fields, extra).items()
                        },
                        "unchanged_runtime_from": str(repo_relative_path(source.config_path)),
                    }
                )

        if not schedule(base):
            state.update(status="stopped", stop_reason="prediction_budget")
            return state
        if base_index is not None:
            remember(base_index, base)

        reference_index = base_index
        state["reference_target"] = state["base_target"]
        if request.reference is not None:
            reference_index = request.space.locate(request.reference.fields)
            if reference_index != base_index:
                if request.reference.label == base.label:
                    raise ValueError("different source and same-resource reference configs need different names")
                state["reference_target"] = {"name": request.reference.label, "hicache": request.reference.fields}
                if not schedule(request.reference):
                    state.update(status="stopped", stop_reason="prediction_budget")
                    update_results(request, state)
                    return state
                remember(reference_index, request.reference)

        rejected = {row["space_index"] for row in state["rejected_candidates"]}
        exhausted = True
        observations = {
            row["space_index"]: None if explore else row["score_sec"]
            for row in state["candidates"]
            if any(work.get("attempted", False) for work in row["workloads"].values())
        }
        initial = (index for index in request.initial_indices if index not in observations)
        proposed = chain(initial, candidate_indices(request.space, request.algorithm, request.seed, observations))
        seen = {base_index, reference_index, *rejected}
        for index in proposed:
            if index in seen:
                continue
            seen.add(index)
            if time.monotonic() - search_started + previous_wall >= request.max_wall_sec:
                exhausted = False
                state["stop_reason"] = "wall_budget"
                break

            label = f"candidate_{index}"
            if label == base.label:
                label += "_target"
            try:
                target = request.space.target(index, label)
            except ValueError as error:
                state["rejected_candidates"].append({"space_index": index, "reason": str(error)})
                continue
            if not schedule(target):
                exhausted = False
                break
            remember(index, target)

        state.update(
            status="predicting",
            stop_reason=state["stop_reason"] or ("space_exhausted" if exhausted else "prediction_budget"),
        )
        source_map = {source.input_id: source for source in prepared.scenario.sources}
        cpu_map = {
            require_repo_path(cpu_service_metadata(path)["source_manifest"]).resolve(): path
            for path in inputs.cpu_service_costs
        }
        for workload, targets in scheduled.items():
            if not targets:
                continue
            width = request.execution["model_run_jobs"]
            for start in range(0, len(targets), width):
                chunk = targets[start : start + width]
                remaining = request.max_wall_sec - previous_wall - (time.monotonic() - search_started)
                if remaining <= 0:
                    state["stop_reason"] = "wall_budget"
                    break

                source = source_map[workload]
                cpu = cpu_map.get(source.manifest_path.resolve())
                selected_inputs = replace(
                    inputs, source_manifests=(source.manifest_path,), cpu_service_costs=(cpu,) if cpu else ()
                )
                directory = output / "predictions" / f"attempt_{len(state['attempts']) + 1}"
                attempt = {
                    "status": "running",
                    "workload": workload,
                    "targets": [target.label for target in chunk],
                    "scheduled_predictions": len(chunk),
                    "charged_predictions": len(chunk),
                    "report": None,
                    "prediction_dir": str(repo_relative_path(directory)),
                    "wall_seconds": None,
                }
                state["attempts"].append(attempt)
                write_json_atomic(report, state)
                outcome = predictor.predict(
                    selected_inputs,
                    tuple(chunk),
                    directory,
                    max_wall_sec=remaining,
                )
                attempt.update(
                    status="finished",
                    return_code=outcome.return_code,
                    wall_seconds=outcome.wall_seconds,
                    charged_predictions=outcome.charged_predictions(len(chunk)),
                )
                if outcome.summary is not None:
                    cells = outcome.summary["prediction"]["cells"]
                    attempt.update(
                        report=str(repo_relative_path(outcome.report_path)),
                        self_charged_predictions=sum(
                            row["attempted"] for row in cells if row["target_config"] == base.label
                        ),
                        source_preparations=outcome.summary["source_preparations"],
                        completed_predictions=sum(row["status"] == "EXECUTED" for row in cells),
                    )
                    if (outcome.return_code in {124, 130, 137} or outcome.return_code < 0) and outcome.launched:
                        attempt["accounting_note"] = "interrupted batch; submitted work is conservatively charged"
                        attempt["self_charged_predictions"] = int(base.label in attempt["targets"])
                else:
                    if outcome.launched:
                        attempt["accounting_note"] = "no current report; submitted work is conservatively charged"
                        attempt["self_charged_predictions"] = int(base.label in attempt["targets"])
                    else:
                        attempt.update(
                            charged_predictions=0, self_charged_predictions=0, accounting_note="deadline before launch"
                        )
                update_results(request, state)
                write_json_atomic(report, state)
                if outcome.return_code in {124, 130}:
                    state["stop_reason"] = "wall_budget" if outcome.return_code == 124 else "interrupted"
                    break
            if state["stop_reason"] in {"wall_budget", "interrupted"}:
                break

        state["status"] = "completed" if state["stop_reason"] == "space_exhausted" else "stopped"
        update_results(request, state)
        if state["status"] == "completed" and not state["fully_evaluated_space"]:
            state["status"] = "completed_with_gaps"
        return state
    except KeyboardInterrupt:
        state.update(status="interrupted", stop_reason="interrupted")
        raise
    except BaseException as error:
        state.update(status="failed", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        if search_started is not None:
            state["search_wall_seconds"] = previous_wall + time.monotonic() - search_started
        state["invocation_wall_seconds"] = time.monotonic() - started
        state["total_wall_seconds"] = previous_total + state["invocation_wall_seconds"]
        state["charged_predictions"] = sum(attempt["charged_predictions"] for attempt in state["attempts"])
        state["self_charged_predictions"] = sum(
            attempt.get("self_charged_predictions", 0) for attempt in state["attempts"]
        )
        preparations = [row for attempt in state["attempts"] for row in attempt.get("source_preparations", [])]
        state["completed_source_preparations"] = len(preparations)
        state["completed_source_preparation_wall_seconds"] = sum(row["wall_seconds"] for row in preparations)
        state.setdefault("confirmation", {"status": "NOT_RUN", "enabled": request.confirmation["enabled"]})
        write_json_atomic(report, state)
