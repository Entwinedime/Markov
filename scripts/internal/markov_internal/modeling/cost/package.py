"""Independent measurement import and offline cost-package assembly."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ...common.io import load_json
from ...common.paths import repo_relative_path, require_repo_path
from ..deployment import require_cost_environment, source_environment
from ..source_dag.profiles import cpu_service_inputs, discover_profile_runs
from .database import CostDatabase, OperationObservation, merge_control_measurements
from .io import service_cost_evidence
from .models import ModelSpec, train_models


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

    specifications = [ModelSpec.from_raw(raw) for raw in specs]
    models, unmeasured_implementations = train_models(specifications, rows)

    ready_models = [model for model in models if model["status"] == "ready"]
    training_captures = {capture for model in ready_models for capture in model["source_captures"]}

    result = {
        "status": "ready" if all(model["status"] == "ready" for model in models) else "missing_data",
        "deployment": request["deployment"],
        "excluded_captures": sorted(excluded),
        "excluded_manifests": sorted(str(repo_relative_path(path)) for path in excluded_manifests),
        "operator_models": ready_models,
        "missing_models": [model for model in models if model["status"] != "ready"],
        "wildcard_unmeasured_implementations": unmeasured_implementations,
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
        training_captures.update(row["capture"] for row in evidence)

    # The import transaction owns original/paired sources and their aliases.
    # Resolve provenance once from that store, not several partly overlapping
    # copies assembled from models and evidence. Prediction never opens the DB.
    result["training_sources"] = sorted(training_captures)
    result["training_source_manifests"] = sorted(database.manifests_for_captures(training_captures))

    return result


def import_evidence(
    database: CostDatabase, evidence: list[dict[str, Any]], *, replace_captures: bool = False
) -> dict[str, Any]:
    """Resolve explicit documents and atomically import their measured rows.

    Normalize control declarations in place before persistence. Controls keep
    their same-capture CPU admission; primitives provide their own observations.
    Physical evidence contributes I/O data, not fabricated operator samples.
    Conflicts roll back the database transaction; raw measurement files remain.
    """

    measurements = []
    for row in evidence:
        payload = row["payload"]
        if row["kind"] == "control_costs":
            if "control_calibrations" in payload:
                payload["control_measurements"] = payload.pop("control_calibrations")
            payload["control_measurements"] = embed_control_measurements(payload["control_measurements"])
            measurements.extend(control_observations(row))
        elif row["kind"] == "cpu_primitive":
            measurements.extend(
                OperationObservation.from_raw(
                    dict(observation, capture=row["capture"], manifest=row["manifest"], deployment=row["deployment"])
                )
                for observation in payload["operator_observations"]
            )

    return database.import_measurements(measurements, evidence, replace_captures=replace_captures)


def control_observations(evidence: dict[str, Any]) -> list[OperationObservation]:
    """Admit exported primitive measurements, not parameters or sample topology.

    Each evidence row describes one original capture. Multiple documents from
    that capture may repeat a primitive; its source position deduplicates it.
    CPU correction belongs to the document's own capture, never the base.
    """

    rows: list[OperationObservation] = []

    def collect(value: dict[str, Any]) -> None:
        if "role" not in value:
            for child in value.values():
                collect(child)
            return

        observations = value.get("operator_observations", [])
        if not observations:
            return
        # Query rows explicitly measure coarse wall intervals, not corrected
        # CPU service. Other primitive exports retain the paired-CPU contract.
        coarse_query = value.get("operation") == "prefetch_query"
        if not coarse_query and (value["cost_basis"] != "paired_cpu_service" or not value["cpu_service_file"]):
            raise ValueError("control operation observations require same-capture CPU correction")
        if require_repo_path(value["source_manifest"]) != require_repo_path(evidence["manifest"]):
            raise ValueError("import control measurements as separate evidence rows for each original capture")

        for observation in observations:
            if coarse_query and (
                observation["operation"] != "prefetch.query"
                or observation["implementation"] != "sglang.storage_hit_min"
                or observation["timing_scope"]
                not in {
                    "before_sample_interval",
                    "sample_to_min_interval",
                    "min_to_apply_interval",
                    "all_reduce_envelope_remainder",
                }
            ):
                raise ValueError("query exports measure control intervals, not pure CPU service")
            rows.append(
                OperationObservation.from_raw(
                    dict(
                        observation,
                        capture=evidence["capture"],
                        manifest=evidence["manifest"],
                        deployment=evidence["deployment"],
                    )
                )
            )

    collect(evidence["payload"].get("control_measurements", {}))
    return rows


def embed_control_measurements(collection: dict[str, Any]) -> dict[str, Any]:
    """Read declared measurement files at import time, not during prediction.

    Policy/page collections retain their selection keys. Leaf documents contain
    measured primitive costs and provenance; source paths remain informational.
    Already embedded documents can be explicitly reimported without raw assets.
    """

    def embed(value: Any) -> Any:
        if isinstance(value, str):
            value = load_json(require_repo_path(value))
        if "role" in value:
            if value.get("cost_basis") == "paired_cpu_service" and "measurement_manifests" not in value:
                correction = load_json(require_repo_path(value["cpu_service_file"]))
                value = dict(value, measurement_manifests=[value["source_manifest"], correction["light_manifest"]])
            elif "measurement_manifests" not in value:
                value = dict(value, measurement_manifests=[value["source_manifest"]])
            return value

        return {key: embed(child) for key, child in value.items()}

    return {key: embed(value) for key, value in collection.items()}


def control_evidence(collection: dict[str, Any], deployment: dict[str, Any]) -> list[dict[str, Any]]:
    """Group explicit control documents by their actual source capture.

    A group configuration and a stringified path collection are not capture
    identities. This uses only the manifests named by measurement documents.
    Selection keys remain intact in database evidence, not runtime inputs.
    """

    captures: dict[str, dict[str, Any]] = {}

    def collect(value: dict[str, Any], keys: tuple[str, ...]) -> None:
        if "role" not in value:
            for key, child in value.items():
                collect(child, (*keys, key))
            return

        source = discover_profile_runs((require_repo_path(value["source_manifest"]),))[0]
        evidence = captures.setdefault(
            source.run_id,
            dict(
                capture=source.run_id,
                manifest=str(repo_relative_path(source.manifest_path)),
                deployment=deployment,
                kind="control_costs",
                payload={"control_measurements": {}},
            ),
        )
        selected = evidence["payload"]["control_measurements"]
        for key in keys[:-1]:
            selected = selected.setdefault(key, {})
        merge_control_measurements(selected, {keys[-1]: value})

    for operation, value in embed_control_measurements(collection).items():
        collect(value, (operation,))

    return list(captures.values())
