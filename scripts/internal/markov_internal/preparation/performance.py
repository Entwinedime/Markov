"""Prepare reusable performance measurements and models, without prediction targets."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field, replace
import math
from pathlib import Path
import time
from typing import Any

from markov_internal.common.io import load_json, write_json
from markov_internal.common.manifest import profile_labels
from markov_internal.common.paths import repo_relative_path, require_repo_path, running_in_modeling_container
from markov_internal.modeling.cost.control import read_control_measurements
from markov_internal.modeling.cost.database import CostDatabase
from markov_internal.modeling.cost.models import estimate, parse_model_specs
from markov_internal.modeling.cost.package import control_evidence, physical_evidence
from markov_internal.modeling.deployment import require_cost_environment, source_environment
from markov_internal.modeling.source_dag.profiles import discover_profile_runs
from markov_internal.profiling.suite import experiment_identity, profile_experiments
from markov_internal.preparation.profile_capture import capture_profile_jobs, profile_capture_job
from markov_internal.preparation.capture import PhysicalBudget, ProfileBudget, pending_group_capture
from markov_internal.preparation.cost_preparation import import_cost_measurements, train_cost_model
from markov_internal.preparation.cpu_service import CpuServiceInputs, cpu_service_plan, prepare_cpu_services
from markov_internal.modeling.source_dag.profiles import cpu_service_metadata
from markov_internal.preparation.performance_retention import retire_performance_traces
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
    retain_raw_traces: bool = False
    sampling_work: dict[str, list[dict[str, Any]]] = field(default_factory=dict)

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
            "retain_raw_traces",
            "sampling_work",
        }
        if unknown := set(raw) - allowed:
            raise ValueError(f"unknown performance inputs; targets and scores are not accepted: {sorted(unknown)}")

        capture_budget = raw.get("capture_budget")
        physical_budget = (raw.get("physical_capture") or {}).get("budget")
        services = frozenset(raw.get("physical_services", []))
        if services - SERVICE_SAMPLERS.keys():
            raise ValueError(f"unsupported physical services: {sorted(services - SERVICE_SAMPLERS.keys())}")
        if not isinstance(raw.get("retain_raw_traces", False), bool):
            raise ValueError("retain_raw_traces must be a boolean")

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
            retain_raw_traces=raw.get("retain_raw_traces", False),
            sampling_work=parse_sampling_work(raw.get("sampling_work", {})),
        )


def parse_sampling_work(raw: Any) -> dict[str, list[dict[str, Any]]]:
    """Declare actual work, not configuration names as fitting coordinates."""

    if not isinstance(raw, dict):
        raise ValueError("sampling_work must map experiment IDs to nonempty operation lists")
    for identity, points in raw.items():
        if not isinstance(identity, str) or not identity or not isinstance(points, list) or not points:
            raise ValueError("sampling_work requires an experiment ID and nonempty operation list")
        for point in points:
            if not isinstance(point, dict) or set(point) != {"operation", "implementation", "timing_scope", "features"}:
                raise ValueError("sampling work needs operation, implementation, timing_scope and features")
            if any(
                not isinstance(point[key], str) or not point[key]
                for key in ("operation", "implementation", "timing_scope")
            ):
                raise ValueError("sampling operation identity must contain nonempty strings")
            features = point["features"]
            if not isinstance(features, dict) or any(
                not isinstance(name, str)
                or not name
                or isinstance(value, bool)
                or not isinstance(value, (int, float))
                or not math.isfinite(value)
                for name, value in features.items()
            ):
                raise ValueError("sampling work features need names and finite numbers")
    return raw


def missing_sampling_work(package: dict[str, Any], points: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """A fitted range alone does not establish that the requested joint work was measured."""

    indexed = {
        (model["operation"], model["implementation"], model["timing_scope"]): model
        for model in package.get("operator_models", [])
    }
    missing = []
    for point in points:
        model = indexed.get(tuple(point[key] for key in ("operation", "implementation", "timing_scope")))
        result = (
            estimate(model, point["features"])
            if model is not None
            else {"status": "missing_data", "reason": "missing_model"}
        )
        if result["status"] == "ready":
            if not result["outside_measured_range"] and not result["unseen_feature_combination"]:
                continue
            result = {"status": "missing_data", "reason": "unmeasured_joint_work"}
        missing.append(dict(point, gap=result))
    return missing


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
    for points in request.sampling_work.values():
        for point in points:
            declared = next(
                (
                    spec
                    for spec in specifications
                    if spec.operation == point["operation"]
                    and spec.timing_scope == point["timing_scope"]
                    and spec.implementation in {"*", point["implementation"]}
                ),
                None,
            )
            if declared is None or set(point["features"]) != set(declared.required_features):
                raise ValueError("sampling work must name a declared model and all its regression/coverage features")
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
        created_profiles: set[Path] = set()
        if request.profile_suite:
            suite = load_json(request.profile_suite)
            if suite["framework"] != "sglang":
                raise ValueError("automatic performance replay currently supports SGLang template workloads only")

            jobs = []
            identities = []
            for index, experiment in enumerate(profile_experiments(suite), start=1):
                config_id, workload = profile_labels(experiment)
                identities.append(experiment_identity(experiment, index))
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
            if len(set(identities)) != len(identities) or len({(job.config_id, job.workload) for job in jobs}) != len(
                jobs
            ):
                raise ValueError("declare one performance experiment per configuration/workload")
            if unknown := request.sampling_work.keys() - set(identities):
                raise ValueError(f"sampling_work names unknown experiments: {sorted(unknown)}")

            measured = {}
            if request.sampling_work and request.database.is_file() and not dry_run:
                refresh_started = time.monotonic()
                refreshed = train_cost_model(database, training, output / "sampling_preflight")
                summary["sampling_preflight_wall_seconds"] = time.monotonic() - refresh_started
                if refreshed is None:
                    summary["status"] = "sampling_preflight_failed"
                    return summary
                measured = load_json(refreshed)

            selected = []
            summary["sampling_plan"] = []
            for identity, job in zip(identities, jobs):
                required = request.sampling_work.get(identity)
                gaps = missing_sampling_work(measured, required) if required else []
                reuse = required is not None and bool(measured) and not gaps
                summary["sampling_plan"].append(
                    {
                        "experiment": identity,
                        "status": "covered_by_database" if reuse else "capture_declared_work",
                        "coverage_declared": required is not None,
                        "missing_work": gaps,
                    }
                )
                if not reuse:
                    selected.append(job)

            captured = capture_profile_jobs(
                selected, output, request.capture_budget, dry_run=dry_run, purpose="performance"
            )
            summary["runtime_capture"] = captured
            if captured["status"] != "profiles_captured":
                summary["status"] = "planned" if dry_run else captured["status"]
                return summary

            manifests.extend(require_repo_path(value) for value in captured["profile_manifests"])
            created_profiles.update(
                require_repo_path(value).resolve() for value in captured["created_profile_manifests"]
            )
        elif request.sampling_work:
            raise ValueError("sampling_work requires profile_suite")

        sources = tuple(discover_profile_runs(tuple(dict.fromkeys(manifests))))
        summary["profile_manifests"] = [str(repo_relative_path(source.manifest_path)) for source in sources]
        cpu = cpu_service_plan(
            sources,
            request.cpu_service,
            output,
            forced_token_bundle=request.forced_token_bundle,
            include_setup_requests=request.include_setup_requests,
        )
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
            import_started = time.monotonic()
            code = import_cost_measurements(request.database, directory, arguments)
            summary["imports"].append(
                {
                    "kind": kind,
                    "return_code": code,
                    "report": str(repo_relative_path(directory / "import_summary.json")),
                    "wall_seconds": time.monotonic() - import_started,
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
            arguments.extend(("--measurement-dir", str(repo_relative_path(output / "runtime_measurements"))))
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

        training_started = time.monotonic()
        model = train_cost_model(database, training, output)
        summary["training_wall_seconds"] = time.monotonic() - training_started
        if model is None:
            summary["status"] = "training_failed"
            return summary

        package = load_json(model)
        sampling_gaps = {
            identity: gaps
            for identity, points in request.sampling_work.items()
            if (gaps := missing_sampling_work(package, points))
        }
        summary.update(
            status="measurement_incomplete" if capture_incomplete or sampling_gaps else package["status"],
            cost_model=str(repo_relative_path(model)),
            ready_operator_models=len(package["operator_models"]),
            missing_operator_models=package["missing_models"],
            missing_io=package.get("io_training", {}).get("missing", []),
            missing_sampling_work=sampling_gaps,
        )
        if request.retain_raw_traces:
            summary["raw_retention"] = {"status": "retained_by_request"}
        elif not capture_incomplete and not sampling_gaps and package["status"] in {"ready", "missing_data"}:
            evidence_to_keep = {}
            if sources:
                imported = load_json(output / "runtime_import" / "import_summary.json")
                evidence_to_keep.update(
                    (
                        require_repo_path(row["source_manifest"]).resolve(),
                        ("operation_samples", require_repo_path(row["measurements"])),
                    )
                    for row in imported["captures"]
                    if require_repo_path(row["source_manifest"]).resolve() in created_profiles
                )
            created_light = {require_repo_path(path).resolve() for path in cpu.get("created_profile_manifests", [])}
            for path in corrections:
                light = require_repo_path(cpu_service_metadata(path)["light_manifest"]).resolve()
                if light in created_light:
                    evidence_to_keep[light] = "cpu_service_cost", path
            summary["raw_retention"] = retire_performance_traces(evidence_to_keep)
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
