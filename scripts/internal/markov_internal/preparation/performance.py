"""Prepare reusable performance measurements and models, without prediction targets."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, replace
from pathlib import Path
import time
from typing import Any

from markov_internal.common.io import load_json, write_json
from markov_internal.common.manifest import profile_labels
from markov_internal.common.paths import repo_relative_path, require_repo_path, running_in_modeling_container
from markov_internal.modeling.cost.control import read_control_measurements
from markov_internal.modeling.cost.database import CostDatabase
from markov_internal.modeling.cost.models import parse_model_specs
from markov_internal.modeling.cost.package import control_evidence, physical_evidence
from markov_internal.modeling.deployment import require_cost_environment, source_environment
from markov_internal.modeling.source_dag.profiles import discover_profile_runs
from markov_internal.profiling.suite import profile_experiments
from markov_internal.preparation.profile_capture import capture_profile_jobs, profile_capture_job
from markov_internal.preparation.capture import PhysicalBudget, ProfileBudget, pending_group_capture
from markov_internal.preparation.cost_preparation import import_cost_measurements, train_cost_model
from markov_internal.preparation.cpu_service import CpuServiceInputs, cpu_service_plan, prepare_cpu_services
from markov_internal.preparation.physical_capture import (
    SERVICE_SAMPLERS,
    PhysicalCaptureInputs,
    admitted_physical_calibration,
    capture_physical,
)


@dataclass(frozen=True)
class PerformanceRequest:
    """Independent maintenance inputs; no prediction targets or score state."""

    database: Path
    training: Path
    output_dir: Path
    profile_suite: Path | None
    profile_manifests: tuple[Path, ...]
    capture_budget: ProfileBudget | None
    cpu_service: CpuServiceInputs
    physical_capture: dict[str, Any]
    physical_calibration: dict[str, Any] | None
    physical_budget: PhysicalBudget | None
    physical_services: frozenset[str]
    control_inputs: dict[str, Any]
    resource_environment: dict[str, str] | None
    forced_token_bundle: str | None
    include_setup_requests: bool
    evidence: Path | None
    observations: Path | None

    @classmethod
    def load(cls, path: Path) -> PerformanceRequest:
        raw = load_json(require_repo_path(path))
        allowed = {
            "database",
            "training",
            "output_dir",
            "profile_suite",
            "profile_manifests",
            "capture_budget",
            "forced_token_bundle",
            "cpu_service_costs",
            "cpu_service_pairs",
            "cpu_service_capture_budget",
            "physical_capture",
            "physical_services",
            "physical_calibration",
            "control_calibrations",
            "resource_environment",
            "include_setup_requests",
            "evidence",
            "observations",
        }
        if unknown := set(raw) - allowed:
            raise ValueError(f"unknown performance inputs; targets and scores are not accepted: {sorted(unknown)}")

        capture_budget = raw.get("capture_budget")
        physical_budget = (raw.get("physical_capture") or {}).get("budget")
        services = frozenset(raw.get("physical_services", []))
        if services - SERVICE_SAMPLERS.keys():
            raise ValueError(f"unsupported physical services: {sorted(services - SERVICE_SAMPLERS.keys())}")

        return cls(
            database=require_repo_path(raw["database"]),
            training=require_repo_path(raw["training"]),
            output_dir=require_repo_path(raw["output_dir"]),
            profile_suite=require_repo_path(raw["profile_suite"]) if raw.get("profile_suite") else None,
            profile_manifests=tuple(require_repo_path(value) for value in raw.get("profile_manifests", [])),
            capture_budget=ProfileBudget.parse(capture_budget) if capture_budget is not None else None,
            cpu_service=CpuServiceInputs.parse(raw),
            physical_capture=raw.get("physical_capture") or {},
            physical_calibration=raw.get("physical_calibration"),
            physical_budget=PhysicalBudget.parse(physical_budget) if physical_budget is not None else None,
            physical_services=services,
            control_inputs=raw.get("control_calibrations", {}),
            resource_environment=raw.get("resource_environment"),
            forced_token_bundle=raw.get("forced_token_bundle"),
            include_setup_requests=bool(raw.get("include_setup_requests", False)),
            evidence=require_repo_path(raw["evidence"]) if raw.get("evidence") else None,
            observations=require_repo_path(raw["observations"]) if raw.get("observations") else None,
        )


def prepare_performance(request: PerformanceRequest, *, dry_run: bool = False) -> dict[str, Any]:
    """Capture declared independent work, import facts, and publish offline models.

    The request owns a persistent database and a sampling budget, not a base or
    target group. Repeating the command reuses completed identical captures and
    deduplicates imports. Updating models is this explicit maintenance action;
    prediction and evaluation cannot trigger it through scores.
    """

    database = str(repo_relative_path(request.database))
    training = load_json(request.training)
    require_cost_environment(training["deployment"])
    specifications = parse_model_specs(training.get("models", []))
    output = request.output_dir

    started = time.monotonic()
    summary: dict[str, Any] = {
        "status": "preparing",
        "database": database,
        "declared_operator_models": len(specifications),
        "scope": "declared independent measurements and models, not target execution or E2E readiness",
        "imports": [],
    }
    try:
        manifests = list(request.profile_manifests)
        if request.profile_suite:
            suite = load_json(request.profile_suite)
            if suite["framework"] != "sglang":
                raise ValueError("automatic performance replay currently supports SGLang template workloads only")

            jobs = []
            for index, experiment in enumerate(profile_experiments(suite), start=1):
                config_id, workload = profile_labels(experiment)
                jobs.append(
                    profile_capture_job(
                        experiment,
                        index,
                        config_id=config_id,
                        workload=workload,
                        forced_token_bundle=request.forced_token_bundle,
                        purpose="performance",
                    )
                )
            if len({(job.config_id, job.workload) for job in jobs}) != len(jobs):
                raise ValueError("declare one performance experiment per configuration/workload")

            captured = capture_profile_jobs(
                jobs, output, request.capture_budget, dry_run=dry_run, purpose="performance"
            )
            summary["runtime_capture"] = captured
            if captured["status"] != "profiles_captured":
                summary["status"] = "planned" if dry_run else captured["status"]
                return summary

            manifests.extend(require_repo_path(value) for value in captured["profile_manifests"])

        sources = tuple(discover_profile_runs(tuple(dict.fromkeys(manifests))))
        summary["profile_manifests"] = [str(repo_relative_path(source.manifest_path)) for source in sources]
        cpu = cpu_service_plan(sources, request.cpu_service, output, forced_token_bundle=request.forced_token_bundle)
        summary["cpu_service"] = cpu
        corrections = prepare_cpu_services(cpu, dry_run=dry_run)
        if cpu["status"] not in {"prepared", "not_requested"}:
            summary["status"] = "planned" if dry_run else cpu["status"]
            return summary

        physical = admitted_physical_calibration(request.physical_calibration)
        capture_incomplete = False
        if request.physical_services:
            if not sources:
                summary.update(
                    status="needs_sampling_deployment", stop_reason="Declare an independent sampling profile/config."
                )
                return summary

            sampling = PhysicalCaptureInputs(
                capture=request.physical_capture,
                calibration=request.physical_calibration,
                resources=request.resource_environment,
                forced_token_bundle=request.forced_token_bundle,
                sources=sources,
                output_dir=output,
                physical_budget=request.physical_budget,
            )
            environments = [source_environment(source, request.resource_environment) for source in sources]
            if any(environment != training["deployment"] for environment in environments):
                raise ValueError("physical sampling profiles must match the training deployment and declared resources")

            captured = capture_physical(sampling, dry_run=dry_run, required_components=request.physical_services)
            summary["physical_capture"] = captured
            if captured["status"] not in {
                "physical_captured",
                "physical_budget_exhausted",
                "needs_physical_calibration",
            }:
                summary["status"] = captured["status"]
                return summary

            capture_incomplete = captured["status"] != "physical_captured"
            physical = replace(sampling).physical

        if dry_run:
            summary["status"] = "planned"
            return summary

        def import_inputs(kind: str, arguments: list[str]) -> bool:
            directory = output / f"{kind}_import"
            code = import_cost_measurements(request.database, directory, arguments)
            summary["imports"].append(
                {
                    "kind": kind,
                    "return_code": code,
                    "report": str(repo_relative_path(directory / "import_summary.json")),
                }
            )
            if code:
                summary["status"] = "import_failed"

            return code == 0

        if sources:
            arguments = [
                item
                for source in sources
                for item in ("--profile-manifest", str(repo_relative_path(source.manifest_path)))
            ]
            arguments.extend(
                item for path in corrections for item in ("--cpu-service-cost", str(repo_relative_path(path)))
            )
            if request.resource_environment is not None:
                path = output / "resource_environment.json"
                write_json(path, request.resource_environment)
                arguments.extend(("--resource-environment", str(repo_relative_path(path))))
            if request.include_setup_requests:
                arguments.append("--include-setup-requests")
            if not import_inputs("runtime", arguments):
                return summary

        evidence = load_json(request.evidence)["evidence"] if request.evidence else []
        if physical and physical.get("measurement_sources"):
            row = physical_evidence(physical, training["deployment"])
            evidence.append(row)
            training["io_evidence"] = [row["capture"]]
        if request.control_inputs:
            controls = read_control_measurements(request.control_inputs)
            environments = {
                measurement.source.manifest_path: source_environment(measurement.source, request.resource_environment)
                for measurement in controls
            }
            if any(environment != training["deployment"] for environment in environments.values()):
                raise ValueError("control measurements must match the training deployment and declared resources")

            evidence.extend(
                control_evidence(controls, training["deployment"], CostDatabase(require_repo_path(database)))
            )
        if evidence:
            path = output / "database_evidence.json"
            write_json(path, {"evidence": evidence})
            if not import_inputs("evidence", ["--evidence", str(repo_relative_path(path))]):
                return summary
        if request.observations and not import_inputs(
            "observations", ["--observations", str(repo_relative_path(request.observations))]
        ):
            return summary

        if not require_repo_path(database).is_file():
            summary.update(
                status="needs_measurements",
                stop_reason="No performance database yet; declare sampling inputs or existing independent measurements.",
            )
            return summary

        model = train_cost_model(database, training, output)
        if model is None:
            summary["status"] = "training_failed"
            return summary

        package = load_json(model)
        summary.update(
            status="measurement_incomplete" if capture_incomplete else package["status"],
            cost_model=str(repo_relative_path(model)),
            ready_operator_models=len(package["operator_models"]),
            missing_operator_models=package["missing_models"],
            missing_io=package.get("io_training", {}).get("missing", []),
        )
        return summary
    except BaseException as error:
        summary.update(status="failed", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        summary["preparation_wall_seconds"] = time.monotonic() - started
        write_json(output / "performance_summary.json", summary)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", required=True, type=Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    if running_in_modeling_container():
        parser.error("performance preparation coordinates capture/modeling containers and runs on the host")

    request = PerformanceRequest.load(args.request)
    output = request.output_dir
    if reason := pending_group_capture(output):
        raise SystemExit(reason)

    result = prepare_performance(request, dry_run=args.dry_run)
    print(f"performance status={result['status']} report={repo_relative_path(output / 'performance_summary.json')}")
    return 0 if result["status"] in {"planned", "ready"} else 2


if __name__ == "__main__":
    raise SystemExit(main())
