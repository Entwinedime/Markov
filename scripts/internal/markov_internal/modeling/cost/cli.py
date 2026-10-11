"""Explicit observation import, offline model training and coverage inspection."""

from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import replace
from pathlib import Path
from typing import Any

from ...common.io import load_json, write_json
from ...common.paths import repo_relative_path, require_repo_path
from ..deployment import require_cost_environment, source_environment
from .database import CostDatabase, OperationObservation, evidence_manifests
from .models import ModelSpec, estimate, train_model
from .io import service_cost_evidence
from .package import control_observations, cpu_service_inputs, embed_control_measurements


def import_profiles(
    database: CostDatabase,
    manifests: list[Path],
    scratch: Path,
    services: list[Path],
    aliases: dict[str, str],
    resources: dict[str, str] | None = None,
    *,
    include_setup_requests: bool = False,
    replace_captures: bool = False,
) -> dict[str, Any]:
    """Extract explicitly selected historical captures, never the prediction base."""

    # Import is a boundary workflow. Prediction and operator inference do not
    # depend on framework acquisition, profile discovery or raw trace readers.
    from ..source_dag.observations import scan_observations
    from ..source_dag.profiles import discover_profile_runs

    sources = discover_profile_runs(tuple(path.resolve() for path in manifests))
    correction = cpu_service_inputs(services, tuple(source.manifest_path.resolve() for source in sources))
    rows = []
    evidence = []
    for source in sources:
        observation = scan_observations(
            source,
            scratch,
            role="history",
            cpu_service=correction.get(source.manifest_path.resolve()),
            include_setup_requests=include_setup_requests,
        )
        manifest = str(repo_relative_path(source.manifest_path))
        # Preserve capture identity across explicit asset aliases. A run's
        # timestamp/identity is provenance; it is never a model feature.
        capture = aliases.get(manifest, source.run_id)
        deployment = source_environment(source, resources)
        observation["resource_environment_basis"] = (
            "explicit_import_declaration" if resources is not None else "capture_metadata_or_unknown"
        )
        for row in observation["operator_observations"]:
            rows.append(
                OperationObservation.from_raw(dict(row, capture=capture, manifest=manifest, deployment=deployment))
            )
        evidence.append(
            dict(capture=capture, manifest=manifest, deployment=deployment, kind="runtime_io", payload=observation)
        )

    return database.import_measurements(rows, evidence, replace_captures=replace_captures)


def train(database: CostDatabase, request: dict[str, Any]) -> dict[str, Any]:
    """Select one deployment and fit declared operators without reading target data."""

    excluded = set(request.get("excluded_captures", []))
    excluded_manifests = request.get("excluded_manifests", [])
    excluded = database.excluded_captures(excluded, excluded_manifests)
    require_cost_environment(request["deployment"])
    rows = database.observations(request["deployment"], excluded_captures=excluded)
    available_evidence = database.evidence(request["deployment"], excluded_captures=excluded)
    specs = request["models"]
    if not specs:
        raise ValueError("training must declare at least one operation model")

    observations_by_operation: dict[str, list[OperationObservation]] = defaultdict(list)
    for row in rows:
        observations_by_operation[row.operation].append(row)

    models = []
    unmeasured_implementations = []
    for raw in specs:
        spec = ModelSpec.from_raw(raw)
        operation_rows = [
            row
            for operation in (spec.observation_operations or (spec.operation,))
            for row in observations_by_operation[operation]
        ]
        if spec.implementation != "*":
            models.append(train_model(spec, operation_rows))
            continue

        implementations = sorted(
            {row.implementation for row in operation_rows if row.timing_scope == spec.timing_scope}
        )
        for implementation in sorted({row.implementation for row in operation_rows} - set(implementations)):
            unmeasured_implementations.append(
                {
                    "operation": spec.operation,
                    "implementation": implementation,
                    "timing_scope": spec.timing_scope,
                    "reason": "missing_timing_scope",
                    "available_timing_scopes": sorted(
                        {row.timing_scope for row in operation_rows if row.implementation == implementation}
                    ),
                }
            )
        # A direct call need not have queued-worker measurements. Wildcards
        # select implementations measured in this scope, not every backend
        # ever seen under the same operation name.
        models.extend(train_model(replace(spec, implementation=name), operation_rows) for name in implementations)
        if not implementations:
            models.append(
                {
                    "operation": spec.operation,
                    "implementation": "*",
                    "timing_scope": spec.timing_scope,
                    "status": "missing_data",
                    "reason": "missing_timing_scope" if operation_rows else "missing_operation",
                    "available_timing_scopes": sorted({row.timing_scope for row in operation_rows}),
                    "features": list(dict.fromkeys((*spec.features, *spec.coverage_features))),
                    "ineligible_measurements": 0,
                }
            )
    keys = [(model["operation"], model["implementation"], model["timing_scope"]) for model in models]
    if len(set(keys)) != len(keys):
        raise ValueError("provide one cost model per operation, implementation and timing scope")

    ready_models = [model for model in models if model["status"] == "ready"]
    operator_captures = {capture for model in ready_models for capture in model["source_captures"]}
    operator_manifests = {manifest for model in ready_models for manifest in model["source_manifests"]}
    operator_manifests.update(
        path for row in available_evidence if row["capture"] in operator_captures for path in evidence_manifests(row)
    )

    result = {
        "status": "ready" if all(model["status"] == "ready" for model in models) else "missing_data",
        "deployment": request["deployment"],
        "excluded_captures": sorted(excluded),
        "excluded_manifests": sorted(str(repo_relative_path(path)) for path in excluded_manifests),
        "operator_models": ready_models,
        "missing_models": [model for model in models if model["status"] != "ready"],
        "wildcard_unmeasured_implementations": unmeasured_implementations,
        "training_sources": sorted(operator_captures),
        "training_source_manifests": sorted(operator_manifests),
        "measurement_count": sum(model["measurement_count"] for model in ready_models),
        "candidate_measurement_count": len(rows),
        "prediction_reads_database": False,
        "base_used_for_online_fit": False,
        "readiness_scope": "declared operator model specifications; target execution and full cost coverage are not implied",
    }
    if "io_evidence" in request:
        wanted = set(request["io_evidence"])
        evidence = [
            row
            for row in available_evidence
            if row["kind"] == "runtime_io" or (row["kind"] == "physical_io" and row["capture"] in wanted)
        ]
        physical = [row for row in evidence if row["kind"] == "physical_io"]
        if len(physical) != 1:
            raise ValueError("select exactly one applicable physical I/O measurement document")
        captures = [dict(row["payload"], role="history") for row in evidence if row["kind"] == "runtime_io"]
        io = service_cost_evidence(physical[0]["payload"], captures)
        if io["missing"]:
            result["status"] = "missing_data"
        result["readiness_scope"] = (
            "declared operator specifications and requested I/O families; target execution and full cost coverage are not implied"
        )
        primitives = physical[0]["payload"]
        result["hicache"] = {
            "storage_batch_pages": primitives["storage_batch_pages"],
            "kv_bytes_per_token_per_rank": primitives["kv_geometry"]["kv_bytes_per_token_per_rank"],
            "resource_lanes": primitives["resource_lanes"],
            "service_models": io["models"],
        }
        used_runtime_manifests = {row["source_manifest"] for row in io["rows"]}
        used_evidence = [
            row for row in evidence if row["kind"] != "runtime_io" or row["manifest"] in used_runtime_manifests
        ]
        result["io_training"] = {
            "missing": io["missing"],
            "sources": io["sources"],
            "measurement_support": io["measurement_support"],
            "captures": sorted({row["capture"] for row in used_evidence}),
            "considered_runtime_captures": sorted({row["capture"] for row in evidence if row["kind"] == "runtime_io"}),
            "source_scope": "I/O parameter contributors; considered captures also affect evidence availability and fallback selection",
        }
        # Keep the package's isolation boundary conservative: failed fits can
        # influence the choice of a physical fallback even without producing
        # coefficients. Per-family provenance above separates those cases.
        result["training_sources"] = sorted(set(result["training_sources"]) | {row["capture"] for row in evidence})
        result["training_source_manifests"] = sorted(
            set(result["training_source_manifests"]) | {path for row in evidence for path in evidence_manifests(row)}
        )

    # Aliases admitted by deduplicating imports are still training provenance.
    # Carry them into the package because prediction never opens the database.
    result["training_source_manifests"] = sorted(
        set(result["training_source_manifests"]) | database.manifests_for_captures(set(result["training_sources"]))
    )

    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    importer = actions.add_parser(
        "import", help="admit declared measurements or explicitly selected historical profiles"
    )
    importer.add_argument("--database", type=Path, required=True)
    inputs = importer.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--observations", type=Path, help="JSON with operator observation rows")
    inputs.add_argument("--profile-manifest", type=Path, action="append")
    inputs.add_argument("--evidence", type=Path, help="JSON with explicit physical_io/control_costs measurements")
    importer.add_argument("--cpu-service-cost", type=Path, action="append", default=[])
    importer.add_argument(
        "--include-setup-requests",
        action="store_true",
        help="include recorded setup requests in independent profile measurements; does not change evaluation windows",
    )
    importer.add_argument("--capture-aliases", type=Path, help="explicit manifest-to-original-capture identity map")
    importer.add_argument(
        "--replace-captures",
        action="store_true",
        help="replace all derived measurements of incoming captures; provide complete inputs and retrain exported packages",
    )
    importer.add_argument(
        "--resource-environment",
        type=Path,
        help="explicit device/host/storage/implementation declaration for selected historical profiles",
    )
    importer.add_argument("--output-dir", type=Path, required=True)

    trainer = actions.add_parser("train", help="fit offline from one declared deployment and operation forms")
    trainer.add_argument("--database", type=Path, required=True)
    trainer.add_argument("--request", type=Path, required=True)
    trainer.add_argument("--output", type=Path, required=True)

    inspector = actions.add_parser(
        "inspect", help="read stored coverage or estimate declared work using a trained model"
    )
    inspector.add_argument("--database", type=Path)
    inspector.add_argument("--model", type=Path)
    inspector.add_argument("--work", type=Path, help="operation descriptors to inspect against --model")
    inspector.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.action == "import" and args.resource_environment and not args.profile_manifest:
        parser.error("--resource-environment annotates explicitly selected --profile-manifest inputs only")
    if args.action == "import" and args.include_setup_requests and not args.profile_manifest:
        parser.error("--include-setup-requests requires --profile-manifest")

    if args.action == "import":
        database = CostDatabase(require_repo_path(args.database))
        output = require_repo_path(args.output_dir)
        if args.evidence:
            rows = load_json(require_repo_path(args.evidence))["evidence"]
            for row in rows:
                if row["kind"] == "control_costs" and "control_calibrations" in row["payload"]:
                    payload = row["payload"]
                    payload["control_measurements"] = embed_control_measurements(payload.pop("control_calibrations"))
                elif row["kind"] == "control_costs":
                    row["payload"]["control_measurements"] = embed_control_measurements(
                        row["payload"]["control_measurements"]
                    )

            measurements = [
                observation
                for row in rows
                if row["kind"] == "control_costs"
                for observation in control_observations(row)
            ]
            for row in rows:
                if row["kind"] != "cpu_primitive":
                    continue
                measurements.extend(
                    OperationObservation.from_raw(
                        dict(
                            observation,
                            capture=row["capture"],
                            manifest=row["manifest"],
                            deployment=row["deployment"],
                        )
                    )
                    for observation in row["payload"]["operator_observations"]
                )
            result = database.import_measurements(measurements, rows, replace_captures=args.replace_captures)
        elif args.observations:
            rows = [
                OperationObservation.from_raw(row)
                for row in load_json(require_repo_path(args.observations))["observations"]
            ]
            result = database.import_measurements(rows, [], replace_captures=args.replace_captures)
        else:
            result = import_profiles(
                database,
                [require_repo_path(path) for path in args.profile_manifest],
                output,
                [require_repo_path(path) for path in args.cpu_service_cost],
                load_json(require_repo_path(args.capture_aliases)) if args.capture_aliases else {},
                load_json(require_repo_path(args.resource_environment)) if args.resource_environment else None,
                include_setup_requests=args.include_setup_requests,
                replace_captures=args.replace_captures,
            )
        write_json(output / "import_summary.json", result)
    elif args.action == "train":
        result = train(CostDatabase(require_repo_path(args.database)), load_json(require_repo_path(args.request)))
        write_json(require_repo_path(args.output), result)
    else:
        if args.database:
            result = CostDatabase(require_repo_path(args.database)).summary()
        elif args.model and args.work:
            models = load_json(require_repo_path(args.model))["operator_models"]
            indexed = {(model["operation"], model["implementation"], model["timing_scope"]): model for model in models}
            results = []
            for work in load_json(require_repo_path(args.work))["operations"]:
                model = indexed.get((work["operation"], work["implementation"], work["timing_scope"]))
                if model is not None:
                    inspection = estimate(model, work["features"])
                else:
                    scopes = sorted(
                        item["timing_scope"]
                        for item in models
                        if (item["operation"], item["implementation"]) == (work["operation"], work["implementation"])
                    )
                    inspection = {
                        "status": "missing_data",
                        "reason": "missing_timing_scope" if scopes else "missing_operation",
                        "available_timing_scopes": scopes,
                    }

                results.append(dict(work=work, cost=inspection))
            result = {
                "status": "missing_data" if any(row["cost"]["status"] != "ready" for row in results) else "ready",
                "scope": "declared work cost queries, not graph construction or E2E readiness",
                "operations": results,
            }
        else:
            parser.error("inspect needs --database or both --model and --work")
        write_json(require_repo_path(args.output), result)

    return 2 if result.get("status") == "missing_data" else 0


if __name__ == "__main__":
    raise SystemExit(main())
