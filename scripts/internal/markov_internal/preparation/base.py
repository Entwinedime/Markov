"""Prepare one base's inputs independently of targets and experiment reports."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import time
from typing import Any

from markov_internal.common.io import load_json, write_json
from markov_internal.common.paths import repo_relative_path
from markov_internal.modeling.context import PredictionInputs
from markov_internal.modeling.deployment import source_environment
from markov_internal.preparation.cpu_service import cpu_service_plan, prepare_cpu_services
from markov_internal.preparation.scenario import ScenarioRequest


@dataclass(frozen=True)
class PreparedInputs:
    """Prepared scenario and report; an incomplete result has no model inputs."""

    scenario: ScenarioRequest
    cpu_service_files: tuple[Path, ...]
    summary: dict[str, Any]

    @property
    def prediction_inputs(self) -> PredictionInputs | None:
        scenario = self.scenario
        if self.summary["status"] != "source_inputs_prepared" or scenario.cost_model is None:
            return None

        return PredictionInputs(
            source_manifests=tuple(source.manifest_path for source in scenario.sources),
            cost_model=scenario.cost_model,
            cpu_service_costs=self.cpu_service_files,
            resource_environment=source_environment(scenario.sources[0], scenario.resource_environment)["resources"],
        )


def prepare_inputs(scenario: ScenarioRequest, *, dry_run: bool = False, prepare_costs: bool = True) -> PreparedInputs:
    """Acquire source/CPU data and select independent costs within budgets.

    No target configuration, target measurement or prediction is needed. With
    prepare_costs=False the caller requests source/CPU preparation only.
    """

    started = time.monotonic()
    services: tuple[Path, ...] = ()
    summary: dict[str, Any] = {"base_config": scenario.base_config, "status": "preparing", "captures": []}
    path = scenario.output_dir / "input_summary.json"
    write_json(path, summary)
    try:
        database_inputs = scenario.cost_database is not None and scenario.cost_training is not None
        if prepare_costs and scenario.cost_model is None and not database_inputs:
            summary.update(
                status="needs_independent_cost_model",
                stop_reason="引用已有 cost_model，或提供 cost_database/cost_training；首次性能维护使用 prepare-costs。",
            )
            return PreparedInputs(scenario, services, summary)

        if scenario.missing_base_workloads:
            # Existing inputs never import the device capture planner.
            from markov_internal.preparation.profile_capture import capture_base

            summary["status"] = "capturing_base"
            write_json(path, summary)
            result = capture_base(scenario, dry_run=dry_run)
            summary["base_plan"] = result
            if result["status"] != "base_captured":
                summary["status"] = "planned" if dry_run else result["status"]
                return PreparedInputs(scenario, services, summary)

            scenario = ScenarioRequest.load(scenario.path)

        summary["base_manifests"] = [str(repo_relative_path(source.manifest_path)) for source in scenario.sources]
        summary["cpu_service"] = cpu_service_plan(
            scenario.sources,
            scenario.cpu_service,
            scenario.output_dir,
            forced_token_bundle=scenario.forced_token_bundle,
        )
        summary["status"] = "preparing_cpu_service"
        write_json(path, summary)
        services = tuple(prepare_cpu_services(summary["cpu_service"], dry_run=dry_run))
        summary["cpu_service_files"] = [str(repo_relative_path(path)) for path in services]
        if summary["cpu_service"]["status"] not in {"prepared", "not_requested"}:
            summary["status"] = summary["cpu_service"]["status"]
            return PreparedInputs(scenario, services, summary)

        if dry_run:
            summary["status"] = "planned"
            return PreparedInputs(scenario, services, summary)

        if prepare_costs and scenario.cost_model is None:
            from markov_internal.preparation.cost_preparation import train_group_cost_model

            summary["status"] = "preparing_costs"
            write_json(path, summary)
            trained = train_group_cost_model(scenario, load_json(scenario.cost_training))
            if trained is None:
                summary["status"] = "shared_training_incomplete"
                return PreparedInputs(scenario, services, summary)

            scenario = trained

        summary["status"] = "source_inputs_prepared"
        return PreparedInputs(scenario, services, summary)
    except KeyboardInterrupt:
        summary["status"] = "interrupted"
        raise
    except BaseException as error:
        summary.update(status="failed", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        summary["input_preparation_wall_seconds"] = time.monotonic() - started
        write_json(path, summary)
