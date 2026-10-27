"""The public search declaration: fixed scenario, finite space and explicit budgets."""

from __future__ import annotations

from dataclasses import dataclass
import math
from pathlib import Path
from typing import Any

from markov_internal.common.io import load_json
from markov_internal.common.paths import require_repo_path
from markov_internal.modeling.types import TargetHiCacheConfig
from markov_internal.modeling.sglang_config import resolve_hicache_fields
from markov_internal.preparation.scenario import ScenarioRequest
from markov_internal.preparation.capture import ProfileBudget
from markov_internal.applications.search.space import SearchSpace


@dataclass(frozen=True)
class SearchRequest:
    path: Path
    scenario: ScenarioRequest
    space: SearchSpace
    reference: TargetHiCacheConfig | None
    weights: dict[str, float]
    algorithm: str
    seed: int | None
    initial_indices: tuple[int, ...]
    max_predictions: int
    max_wall_sec: float
    execution: dict[str, Any]
    confirmation: dict[str, Any]
    confirmation_budget: ProfileBudget | None
    output_dir: Path
    basis: dict[str, Any]

    @classmethod
    def load(cls, path: Path) -> SearchRequest:
        raw = load_json(path)
        if set(raw) - {
            "scenario",
            "target_template",
            "reference_config",
            "space",
            "objective",
            "search",
            "execution",
            "confirmation",
            "output_dir",
        }:
            raise ValueError("unknown search inputs; target traces and scores are not prediction inputs")

        scenario = ScenarioRequest.load(require_repo_path(raw["scenario"]))
        if scenario.automatic_cost_acquisition:
            raise ValueError(
                "search keeps one cost basis; resolve shared demands in preparation before a new comparison"
            )

        template = TargetHiCacheConfig.load(require_repo_path(raw["target_template"]))
        space = SearchSpace.from_raw(template, raw["space"])
        reference = None
        if "reference_config" in raw:
            declared = TargetHiCacheConfig.load(require_repo_path(raw["reference_config"]))
            reference = TargetHiCacheConfig(declared.label, resolve_hicache_fields(declared.fields))
            if space.locate(reference.fields) is None:
                raise ValueError("reference_config must be an execution point in the declared search space")
        objective = raw["objective"]
        if set(objective) - {"metric", "workload", "weights"} or objective["metric"] != "http_e2e_sec":
            raise ValueError("search objective is full HTTP E2E in seconds")
        if ("workload" in objective) == ("weights" in objective):
            raise ValueError("select one workload or explicit workload weights")

        weights = objective["weights"] if "weights" in objective else {objective["workload"]: 1.0}
        if (
            any(
                isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0
                for value in weights.values()
            )
            or not math.isfinite(sum(weights.values()))
            or sum(weights.values()) <= 0
        ):
            raise ValueError("workload weights must be finite, nonnegative, with a positive sum")
        if set(weights) - set(scenario.workload_ids):
            raise ValueError("each weighted workload needs its own declared base")

        total = sum(weights.values())
        weights = {name: value / total for name, value in sorted(weights.items()) if value > 0}
        search = raw["search"]
        if set(search) - {"algorithm", "seed", "max_predictions", "max_wall_sec", "initial_indices"}:
            raise ValueError("unknown search options")
        algorithm = search["algorithm"]
        if algorithm not in {"enumerate", "random", "stratified_random", "local", "coordinate"}:
            raise ValueError("algorithm must be enumerate, random, stratified_random, local, or coordinate")
        seed = search.get("seed")
        initial_indices = tuple(dict.fromkeys(search.get("initial_indices", [])))
        if any(
            isinstance(index, bool) or not isinstance(index, int) or not 0 <= index < space.count
            for index in initial_indices
        ):
            raise ValueError("initial_indices must identify execution points inside the search space")

        if algorithm != "enumerate" and (isinstance(seed, bool) or not isinstance(seed, int)):
            raise ValueError("random search requires an explicit integer seed")
        count = search["max_predictions"]
        wall = search["max_wall_sec"]
        if isinstance(count, bool) or not isinstance(count, int) or count < 0:
            raise ValueError("max_predictions is a nonnegative configuration/workload attempt count")
        if isinstance(wall, bool) or not isinstance(wall, (int, float)) or not math.isfinite(wall) or wall < 0:
            raise ValueError("max_wall_sec needs finite nonnegative seconds")

        execution = {
            "model_run_jobs": 1,
            "trace_threads": 1,
            "trace_file_threads": 1,
            "diagnostics": "off",
            **raw.get("execution", {}),
        }
        if set(execution) - {
            "model_run_jobs",
            "trace_threads",
            "trace_file_threads",
            "diagnostics",
            "experiment_reports",
        }:
            raise ValueError("unknown search execution options")
        for key in ("model_run_jobs", "trace_threads", "trace_file_threads"):
            value = execution[key]
            if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
                raise ValueError(f"{key} must be a positive integer")
        if execution["diagnostics"] not in {"off", "full"}:
            raise ValueError("diagnostics must be off or full")
        if "experiment_reports" in execution and not isinstance(execution["experiment_reports"], bool):
            raise ValueError("experiment_reports must be a boolean")

        confirmation = raw.get("confirmation", {"enabled": False})
        if set(confirmation) - {
            "enabled",
            "profile_suites",
            "target_profile_manifests",
            "budget",
            "top_count",
            "repetitions",
            "screening_repetitions",
        }:
            raise ValueError("unknown normal-confirmation options")
        if not isinstance(confirmation["enabled"], bool):
            raise ValueError("confirmation.enabled must be a boolean")
        confirmation_budget = None
        if confirmation["enabled"]:
            automatic = not confirmation.get("profile_suites") and not confirmation.get("target_profile_manifests")
            if (automatic or confirmation.get("profile_suites")) and "budget" not in confirmation:
                raise ValueError("normal confirmation capture requires its own cumulative budget")
            if automatic or confirmation.get("profile_suites"):
                confirmation_budget = ProfileBudget.parse(confirmation["budget"])
            top = confirmation.get("top_count", 1)
            if isinstance(top, bool) or not isinstance(top, int) or top <= 0:
                raise ValueError("confirmation.top_count must be a positive integer")
            for name in ("repetitions", "screening_repetitions"):
                value = confirmation.get(name, 5 if name == "repetitions" else 1)
                if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
                    raise ValueError(f"confirmation.{name} must be a positive integer")

        declaration = load_json(scenario.path)
        basis = {
            "scenario": {
                key: value
                for key, value in declaration.items()
                if key
                not in {"output_dir", "target_configs", "budget", "base_capture_budget", "cpu_service_capture_budget"}
            },
            "template": template.fields,
            "reference": {"name": reference.label, "hicache": reference.fields} if reference else None,
            "space": raw["space"],
            "weights": weights,
            "algorithm": algorithm,
            "seed": seed,
        }
        if initial_indices:
            basis["initial_indices"] = list(initial_indices)

        return cls(
            path,
            scenario,
            space,
            reference,
            weights,
            algorithm,
            seed,
            initial_indices,
            count,
            float(wall),
            execution,
            confirmation,
            confirmation_budget,
            require_repo_path(raw["output_dir"]),
            basis,
        )
