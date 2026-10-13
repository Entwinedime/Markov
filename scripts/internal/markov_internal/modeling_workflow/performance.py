"""Prepare reusable performance measurements and models, without prediction targets."""

from __future__ import annotations

import argparse
from dataclasses import replace
from pathlib import Path
import time
from typing import Any

from ..common.io import load_json, write_json
from ..common.manifest import profile_labels
from ..common.paths import ROOT_DIR, repo_relative_path, require_repo_path, running_in_modeling_container
from ..common.process import run_command
from ..modeling.cost.control import read_control_measurements
from ..modeling.cost.database import CostDatabase
from ..modeling.cost.models import parse_model_specs
from ..modeling.cost.package import control_evidence, physical_evidence
from ..modeling.deployment import require_cost_environment, source_environment
from ..modeling.source_dag.profiles import discover_profile_runs
from ..profiling.suite import profile_experiments
from .profile_capture import capture_profile_jobs, profile_capture_job
from .capture import PhysicalBudget, ProfileBudget, pending_group_capture
from .cost_preparation import train_cost_model
from .cpu_service import cpu_service_plan, prepare_cpu_services
from .physical_capture import (
    SERVICE_SAMPLERS,
    PhysicalCaptureInputs,
    admitted_physical_calibration,
    capture_physical,
)


def prepare_performance(request_path: Path, *, dry_run: bool = False) -> dict[str, Any]:
    """Capture declared independent work, import facts, and publish offline models.

    The request owns a persistent database and a sampling budget, not a base or
    target group. Repeating the command reuses completed identical captures and
    deduplicates imports. Updating models is this explicit maintenance action;
    prediction and evaluation cannot trigger it through scores.
    """

    raw = load_json(request_path)
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

    database = str(repo_relative_path(require_repo_path(raw["database"])))
    training = load_json(require_repo_path(raw["training"]))
    require_cost_environment(training["deployment"])
    specifications = parse_model_specs(training.get("models", []))
    output = require_repo_path(raw["output_dir"])
    budget = ProfileBudget.parse(raw["capture_budget"]) if raw.get("capture_budget") is not None else None
    if raw.get("cpu_service_capture_budget") is not None:
        ProfileBudget.parse(raw["cpu_service_capture_budget"])
    physical_raw = (raw.get("physical_capture") or {}).get("budget")
    physical_budget = PhysicalBudget.parse(physical_raw) if physical_raw is not None else None
    services = set(raw.get("physical_services", []))
    if services - SERVICE_SAMPLERS.keys():
        raise ValueError(f"unsupported physical services: {sorted(services - SERVICE_SAMPLERS.keys())}")

    started = time.monotonic()
    summary: dict[str, Any] = {
        "status": "preparing",
        "database": database,
        "declared_operator_models": len(specifications),
        "scope": "declared independent measurements and models, not target execution or E2E readiness",
        "imports": [],
    }
    try:
        manifests = [require_repo_path(value) for value in raw.get("profile_manifests", [])]
        if raw.get("profile_suite"):
            suite = load_json(require_repo_path(raw["profile_suite"]))
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
                        forced_token_bundle=raw.get("forced_token_bundle"),
                        purpose="performance",
                    )
                )
            if len({(job.config_id, job.workload) for job in jobs}) != len(jobs):
                raise ValueError("declare one performance experiment per configuration/workload")

            captured = capture_profile_jobs(jobs, output, budget, dry_run=dry_run, purpose="performance")
            summary["runtime_capture"] = captured
            if captured["status"] != "profiles_captured":
                summary["status"] = "planned" if dry_run else captured["status"]
                return summary

            manifests.extend(require_repo_path(value) for value in captured["profile_manifests"])

        sources = tuple(discover_profile_runs(tuple(dict.fromkeys(manifests))))
        summary["profile_manifests"] = [str(repo_relative_path(source.manifest_path)) for source in sources]
        cpu = cpu_service_plan(sources, raw, output)
        summary["cpu_service"] = cpu
        corrections = prepare_cpu_services(cpu, dry_run=dry_run)
        if cpu["status"] not in {"prepared", "not_requested"}:
            summary["status"] = "planned" if dry_run else cpu["status"]
            return summary

        physical = admitted_physical_calibration(raw.get("physical_calibration"))
        capture_incomplete = False
        if services:
            if not sources:
                summary.update(
                    status="needs_sampling_deployment", stop_reason="Declare an independent sampling profile/config."
                )
                return summary

            sampling = PhysicalCaptureInputs(
                capture=raw.get("physical_capture") or {},
                calibration=raw.get("physical_calibration"),
                resources=raw.get("resource_environment"),
                forced_token_bundle=raw.get("forced_token_bundle"),
                sources=sources,
                output_dir=output,
                physical_budget=physical_budget,
            )
            environments = [source_environment(source, raw.get("resource_environment")) for source in sources]
            if any(environment != training["deployment"] for environment in environments):
                raise ValueError("physical sampling profiles must match the training deployment and declared resources")

            captured = capture_physical(sampling, dry_run=dry_run, required_components=services)
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
            code = run_command(
                [
                    str(ROOT_DIR / "scripts/model.sh"),
                    "cost-db",
                    "import",
                    "--database",
                    database,
                    "--output-dir",
                    str(repo_relative_path(directory)),
                    *arguments,
                ]
            ).returncode
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
            if raw.get("resource_environment") is not None:
                path = output / "resource_environment.json"
                write_json(path, raw["resource_environment"])
                arguments.extend(("--resource-environment", str(repo_relative_path(path))))
            if raw.get("include_setup_requests", False):
                arguments.append("--include-setup-requests")
            if not import_inputs("runtime", arguments):
                return summary

        evidence = load_json(require_repo_path(raw["evidence"]))["evidence"] if raw.get("evidence") else []
        if physical and physical.get("measurement_sources"):
            row = physical_evidence(physical, training["deployment"])
            evidence.append(row)
            training["io_evidence"] = [row["capture"]]
        if raw.get("control_calibrations"):
            controls = read_control_measurements(raw["control_calibrations"])
            environments = {
                measurement.source.manifest_path: source_environment(
                    measurement.source, raw.get("resource_environment")
                )
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
        if raw.get("observations") and not import_inputs("observations", ["--observations", raw["observations"]]):
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

    path = require_repo_path(args.request)
    output = require_repo_path(load_json(path)["output_dir"])
    if reason := pending_group_capture(output):
        raise SystemExit(reason)

    result = prepare_performance(path, dry_run=args.dry_run)
    print(f"performance status={result['status']} report={repo_relative_path(output / 'performance_summary.json')}")
    return 0 if result["status"] in {"planned", "ready"} else 2


if __name__ == "__main__":
    raise SystemExit(main())
