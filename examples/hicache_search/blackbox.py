"""Research-only SMAC baseline: real normal runs, with no predictor or cost fitting."""

from __future__ import annotations

import argparse
from pathlib import Path
import statistics
import time
from typing import Any

from ConfigSpace import ConfigurationSpace
from smac import HyperparameterOptimizationFacade, Scenario
from smac.runhistory.dataclasses import TrialValue
from smac.runhistory.enumerations import StatusType

from markov_internal.applications.search.request import SearchRequest
from markov_internal.common.commands import positive_int
from markov_internal.common.io import load_json, write_json
from markov_internal.common.manifest import workload_report_path
from markov_internal.common.paths import repo_relative_path, require_repo_path
from markov_internal.modeling.sglang_config import (
    TIMEOUT_FIELDS,
    canonical_hicache,
    hicache_workload_spec,
    resolve_hicache_fields,
)
from markov_internal.modeling.types import TargetHiCacheConfig
from markov_internal.modeling.workload import load_workload_window
from markov_internal.preparation.profile_capture import (
    normal_confirmation_job,
    profile_token_plan,
    capture_profile_jobs,
)
from markov_internal.preparation.resources import wait_for_normal_resources


def target_from_configuration(request: SearchRequest, values: dict[str, Any], name: str) -> TargetHiCacheConfig:
    """Keep reference token capacities; SMAC varies the declared seven policy features."""

    reference = request.reference.fields
    page = int(values["page_size"])
    fields = {
        **reference,
        "page_size": page,
        "l1_capacity_pages": reference["l1_capacity_pages"] * reference["page_size"] // page,
        "l2_capacity_pages": reference["l2_capacity_pages"] * reference["page_size"] // page,
        "write_policy": values["write_policy"],
        "prefetch_policy": values["prefetch_policy"],
        "prefetch_threshold_pages": int(values["prefetch_threshold_tokens"]) // page,
    }
    fields.update({key: float(values[key]) for key in TIMEOUT_FIELDS if key in values})
    fields = resolve_hicache_fields(fields)
    if request.space.locate(fields) is None:
        raise ValueError("SMAC configuration is outside the declared prediction comparison space")

    return TargetHiCacheConfig(name, fields)


def optimizer_for(
    space: ConfigurationSpace, output: Path, trials: int, seed: int, failure_cost_sec: float
) -> HyperparameterOptimizationFacade:
    """Use the declared reference default first and permit one observation per configuration."""

    scenario = Scenario(space, output_directory=output, n_trials=trials, seed=seed, crash_cost=failure_cost_sec)
    initial = HyperparameterOptimizationFacade.get_initial_design(
        scenario, n_configs=0, additional_configs=[space.get_default_configuration()]
    )
    return HyperparameterOptimizationFacade(
        scenario,
        target_function=None,
        initial_design=initial,
        intensifier=HyperparameterOptimizationFacade.get_intensifier(scenario, max_config_calls=1),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", type=Path, required=True)
    parser.add_argument("--configspace", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--trials", type=positive_int, default=6)
    args = parser.parse_args()
    started = time.monotonic()
    request = SearchRequest.load(require_repo_path(args.request))
    if request.reference is None or request.confirmation_budget is None or len(request.weights) != 1:
        parser.error("this baseline requires one workload, an explicit same-resource reference and a normal budget")

    output = require_repo_path(args.output_dir)
    report = output / "blackbox_summary.json"
    if report.exists():
        parser.error("use a new output; interrupted observations and cumulative capture usage must be reconciled first")

    source = next(source for source in request.scenario.sources if source.input_id in request.weights)
    token_plan = profile_token_plan(source, request.scenario.forced_token_bundle)
    if token_plan is None:
        parser.error("normal comparisons require the prepared source's recorded replay tokens")

    state = {
        "status": "running",
        "request": str(repo_relative_path(request.path)),
        "algorithm": "SMAC",
        "seed": request.seed,
        "screening_attempt_limit": args.trials,
        "observations": [],
        "optimizer_wall_seconds": 0.0,
        "optimizer_failure_penalty_sec": request.confirmation_budget.wall_seconds,
        "scope": "Full HTTP normal measurements only; no prediction, target trace or cost fitting.",
    }

    def measure(target: TargetHiCacheConfig, phase: str, repetition: int) -> float | None:
        specs = output / "normal_configs.json"
        write_json(specs, {"configs": [hicache_workload_spec(target.label, target.fields)]})
        job = normal_confirmation_job(
            source,
            target,
            output,
            specs,
            token_plan,
            repetition=repetition,
            index=len(state["observations"]) + 1,
            phase=phase,
        )
        wait_for_normal_resources(output)
        result = capture_profile_jobs([job], output, request.confirmation_budget, dry_run=False, purpose="confirmation")
        row = {"target": target.label, "hicache": target.fields, "phase": phase, "status": result["status"]}
        state["observations"].append(row)
        write_json(report, state)
        if result["status"] == "failed":
            return None
        if result["status"] != "profiles_captured":
            raise RuntimeError(f"normal capture incomplete: {result['status']}; retain and inspect its ledger")

        (manifest,) = result["profile_manifests"]
        window = load_workload_window(workload_report_path(load_json(require_repo_path(manifest))))
        if window is None or window.source != "workload_report.formal_window":
            raise ValueError("normal measurement lacks the required formal HTTP window")
        row.update(manifest=manifest, http_e2e_sec=window.actual_e2e_ns / 1e9)
        write_json(report, state)
        return row["http_e2e_sec"]

    try:
        space = ConfigurationSpace.from_json(require_repo_path(args.configspace))
        failure_cost_sec = request.confirmation_budget.wall_seconds
        optimizer = optimizer_for(space, output / "smac", args.trials, request.seed, failure_cost_sec)
        choices = []
        for index in range(args.trials):
            tick = time.monotonic()
            trial = optimizer.ask()
            state["optimizer_wall_seconds"] += time.monotonic() - tick
            target = target_from_configuration(request, dict(trial.config), f"smac_trial_{index + 1}")
            tick = time.monotonic()
            cost = measure(target, "blackbox_screening", 1)
            elapsed = time.monotonic() - tick
            tick = time.monotonic()
            # A finite budget-derived penalty keeps SMAC trainable; it is never a measured duration.
            optimizer.tell(
                trial,
                TrialValue(
                    cost=cost if cost is not None else failure_cost_sec,
                    time=elapsed,
                    status=StatusType.SUCCESS if cost is not None else StatusType.CRASHED,
                ),
            )
            state["optimizer_wall_seconds"] += time.monotonic() - tick
            if cost is not None:
                choices.append((cost, index, target))

        if not choices:
            state["status"] = "NO_VALID_SCREENING_MEASUREMENTS"
            return 2
        winner = min(choices, key=lambda row: row[:2])[2]
        reference = request.reference
        if canonical_hicache(winner.fields) == canonical_hicache(reference.fields):
            winner = reference
        state["selected_target"] = {"name": winner.label, "hicache": winner.fields}
        write_json(output / "validation_selection.json", state["selected_target"])
        targets = [reference] if winner is reference else [reference, winner]
        samples = {target.label: [] for target in targets}
        failed_targets = set()
        repetitions = request.confirmation.get("repetitions", 5)
        for repetition in range(repetitions):
            rotated = targets[repetition % len(targets) :] + targets[: repetition % len(targets)]
            for target in rotated:
                if target.label in failed_targets:
                    continue

                cost = measure(target, "blackbox_validation", repetition + 1)
                if cost is None:
                    failed_targets.add(target.label)
                    continue

                samples[target.label].append(cost)
        state["validation"] = {
            name: {
                "samples_sec": values,
                "complete": name not in failed_targets and len(values) == repetitions,
                "mean_sec": statistics.mean(values)
                if name not in failed_targets and len(values) == repetitions
                else None,
                "range_sec": [min(values), max(values)] if values else None,
            }
            for name, values in samples.items()
        }
        state["failed_validation_targets"] = sorted(failed_targets)
        if reference.label in failed_targets:
            state.update(status="MEASUREMENT_INCOMPLETE", recommended_target=None, confirmed_time_reduction=None)
            return 2

        winner_complete = winner.label not in failed_targets
        separated = (
            winner_complete and winner is not reference and max(samples[winner.label]) < min(samples[reference.label])
        )
        state.update(
            status="CONFIRMED_IMPROVEMENT" if separated else "NO_CONFIRMED_IMPROVEMENT",
            recommended_target=winner.label if separated else None,
            confirmed_time_reduction=(
                1 - statistics.mean(samples[winner.label]) / statistics.mean(samples[reference.label])
                if winner_complete
                else None
            ),
        )
        return 0
    except BaseException as error:
        state.update(status="INTERRUPTED" if isinstance(error, KeyboardInterrupt) else "FAILED", error=str(error))
        raise
    finally:
        state["wall_seconds"] = time.monotonic() - started
        write_json(report, state)


if __name__ == "__main__":
    raise SystemExit(main())
