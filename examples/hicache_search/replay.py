"""Independent finite-table selection replay; never actual device-tuning timing."""

from __future__ import annotations

import argparse
from pathlib import Path
import random
from statistics import mean
import time
from typing import Any

from ConfigSpace import ConfigurationSpace
from smac.runhistory.dataclasses import TrialValue
from smac.runhistory.enumerations import StatusType

from examples.hicache_search.blackbox import optimizer_for, target_from_configuration
from markov_internal.applications.search.request import SearchRequest
from markov_internal.common.io import load_json, write_json
from markov_internal.common.manifest import profile_labels, workload_report_path
from markov_internal.common.paths import require_repo_path
from markov_internal.modeling.sglang_config import canonical_hicache
from markov_internal.modeling.workload import load_workload_window


def measured_table(targets: list[dict], attempts: list[dict], repetitions: int) -> dict[str, dict[str, Any]]:
    """Preserve partial and failed blocks; only complete distinct repetitions have a mean."""

    table = {target["name"]: {**target, "attempts": []} for target in targets}
    for attempt in attempts:
        name, _ = profile_labels(attempt["inputs"]["suite"])
        row = {
            "status": attempt["status"],
            "repetition": attempt["inputs"]["suite"]["metadata"]["normal_repetition"],
        }
        if attempt["status"] == "completed":
            manifest = attempt["profile_manifest"]
            window = load_workload_window(workload_report_path(load_json(require_repo_path(manifest))))
            if window is None or window.source != "workload_report.formal_window" or window.actual_e2e_ns <= 0:
                raise ValueError(f"Invalid formal HTTP reference window: {manifest}")
            row.update(manifest=manifest, http_e2e_sec=window.actual_e2e_ns / 1e9)
        table[name]["attempts"].append(row)

    for row in table.values():
        samples = row["attempts"]
        complete = (
            len(samples) == repetitions
            and {item["repetition"] for item in samples} == set(range(1, repetitions + 1))
            and all(item["status"] == "completed" for item in samples)
        )
        row["complete"] = complete
        row["mean_sec"] = mean(item["http_e2e_sec"] for item in samples) if complete else None
    return table


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    root = require_repo_path(args.root)
    plan = load_json(root / "inputs/finite_reference_analysis_plan.json")
    # Replay cannot influence either the live black-box sequence or the main selections.
    live = load_json(root / "blackbox/blackbox_summary.json")
    if live["status"] not in {
        "CONFIRMED_IMPROVEMENT",
        "NO_CONFIRMED_IMPROVEMENT",
        "NO_VALID_SCREENING_MEASUREMENTS",
        "MEASUREMENT_INCOMPLETE",
    }:
        parser.error("Live SMAC has not finished; finite-table replay must wait")
    passed = load_json(root / "finite_reference/normal/pass_summary.json")
    if passed["status"] not in {"completed", "completed_with_failures"}:
        parser.error("The declared reference capture pass is incomplete")

    started = time.monotonic()
    declaration = load_json(require_repo_path(plan["targets"]))
    ledger = load_json(root / "finite_reference/normal/confirmation_capture_ledger.json")
    table = measured_table(declaration["targets"], ledger["attempts"], declaration["repetitions"])
    usable = [row["mean_sec"] for row in table.values() if row["complete"]]
    best = min(usable) if usable else None
    request = SearchRequest.load(root / "inputs/final/competitive_revisit_tight.json")
    failure_cost_sec = request.confirmation_budget.wall_seconds
    canonical_names = {canonical_hicache(row["hicache"]): name for name, row in table.items()}
    reference = canonical_names[canonical_hicache(request.reference.fields)]
    order = list(table)
    cells = load_json(root / "finite_reference/prediction/workflow_summary.json")["prediction"]["cells"]
    predictions = {row["target_config"]: row["http_e2e_us"] for row in cells if row["status"] == "EXECUTED"}
    guided = [
        reference,
        *sorted(
            (name for name in predictions if name != reference), key=lambda name: (predictions[name], order.index(name))
        ),
    ]
    trials = []
    for seed in plan["seed_values"]:
        for method in plan["methods"]:
            choices = []
            if method == "random":
                choices = [name for name in order if name != reference]
                random.Random(seed).shuffle(choices)
                choices.insert(0, reference)
            elif method == "model_guided_screening":
                choices = guided
            elif method == "SMAC":
                space = ConfigurationSpace.from_json(require_repo_path(plan["configspace"]))
                optimizer = optimizer_for(
                    space,
                    root / "offline/smac" / str(seed),
                    plan["queried_configurations_per_seed"],
                    seed,
                    failure_cost_sec,
                )
            else:
                raise ValueError(f"Unknown predeclared replay method: {method}")

            observed = []
            for index in range(plan["queried_configurations_per_seed"]):
                if method == "SMAC":
                    trial = optimizer.ask()
                    target = target_from_configuration(request, dict(trial.config), "offline_query")
                    name = canonical_names[canonical_hicache(target.fields)]
                else:
                    if index >= len(choices):
                        break
                    name = choices[index]
                if index == 0 and name != reference:
                    raise ValueError("Every replay method must charge the same initial reference query")

                tick = time.monotonic()
                value = table[name]["mean_sec"]
                if value is not None:
                    observed.append((value, name))
                if method == "SMAC":
                    optimizer.tell(
                        trial,
                        TrialValue(
                            cost=value if value is not None else failure_cost_sec,
                            time=time.monotonic() - tick,
                            status=StatusType.SUCCESS if value is not None else StatusType.CRASHED,
                        ),
                    )
                incumbent = min(observed) if observed else None
                trials.append(
                    {
                        "method": method,
                        "seed": seed,
                        "query": index + 1,
                        "charged_normal_samples": (index + 1) * plan["charged_normal_samples_per_query"],
                        "queried_target": name,
                        "revealed_mean_sec": value,
                        "selected_target": incumbent[1] if incumbent else None,
                        "selected_mean_sec": incumbent[0] if incumbent else None,
                        "loss_vs_complete_usable_reference": (incumbent[0] - best) / best
                        if incumbent and best is not None
                        else None,
                    }
                )
    write_json(
        root / "offline/selection_replay.json",
        {
            "declared_targets": len(table),
            "complete_usable_targets": len(usable),
            "reference_coverage_complete": len(usable) == len(table),
            "best_complete_usable_mean_sec": best,
            "table": table,
            "trials": trials,
            "offline_wall_sec": time.monotonic() - started,
            "optimizer_failure_penalty_sec": failure_cost_sec,
            "scope": plan["scope"],
            "limitations": "Post-hoc finite-table selection only. Partial or failed blocks remain unavailable and charged. Model ranking has its own prediction budget. Replay wall is not actual tuning time; sample ranges are not confidence intervals.",
        },
    )


if __name__ == "__main__":
    main()
