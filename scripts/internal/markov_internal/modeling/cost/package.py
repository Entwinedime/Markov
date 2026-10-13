"""Independent measurement import and offline cost-package assembly."""

from __future__ import annotations

from collections import defaultdict
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from ...common.io import load_json
from ...common.paths import repo_relative_path, require_repo_path
from ..deployment import require_cost_environment, source_environment
from ..source_dag.profiles import cpu_service_inputs, discover_profile_runs
from .control import ControlMeasurement, control_entries
from .database import CostDatabase, OperationObservation
from .io import service_cost_evidence
from .models import ModelSpec, parse_model_specs, train_models
from .physical import read_physical_measurements


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
        # A suite-local run_id can repeat across independent captures. Keep
        # existing provenance on reimport; new identity follows its manifest.
        # Explicit aliases handle renamed copies, never fitting variables.
        capture = aliases[manifest] if manifest in aliases else database.capture_identity(manifest)
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
    specs = request.get("models", [])
    if not specs and "io_evidence" not in request:
        raise ValueError("training must declare operation models or physical I/O evidence")

    specifications = parse_model_specs(specs)
    families: dict[tuple[str, ...], list[ModelSpec]] = defaultdict(list)
    for specification in specifications:
        families[tuple(sorted(specification.source_operations))].append(specification)

    models, unmeasured_implementations = [], []
    for operations, family_specs in families.items():
        rows = database.observations(request["deployment"], operations=operations, excluded_captures=excluded)
        fitted, missing = train_models(family_specs, rows)
        models.extend(fitted)
        unmeasured_implementations.extend(missing)
        del rows

    order = {
        (spec.operation, spec.implementation, spec.timing_scope): index for index, spec in enumerate(specifications)
    }

    def model_order(model: dict[str, Any]) -> int:
        identity = model["operation"], model["implementation"], model["timing_scope"]
        if identity in order:
            return order[identity]
        return order[model["operation"], "*", model["timing_scope"]]

    models.sort(key=model_order)
    unmeasured_implementations.sort(key=model_order)
    keys = [(model["operation"], model["implementation"], model["timing_scope"]) for model in models]
    if len(set(keys)) != len(keys):
        raise ValueError("provide one cost model per operation, implementation and timing scope")

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
        "candidate_measurement_count": database.measurement_count(request["deployment"], excluded_captures=excluded),
        "readiness_scope": "declared operator model specifications; target execution and full cost coverage are not implied",
    }
    if "io_evidence" in request:
        available_evidence = database.evidence(request["deployment"], excluded_captures=excluded)
        wanted = set(request["io_evidence"])
        evidence = [
            row
            for row in available_evidence
            if row["kind"] == "runtime_io" or (row["kind"] == "physical_io" and row["capture"] in wanted)
        ]
        physical = [row for row in evidence if row["kind"] == "physical_io"]
        if len(physical) > 1:
            raise ValueError("select exactly one applicable physical I/O measurement document")
        if not physical:
            result.update(
                status="missing_data",
                io_training={
                    "missing": [{"component": "physical", "reason": "missing_selected_physical_evidence"}],
                    "captures": [],
                },
                training_sources=sorted(training_captures),
                training_source_manifests=sorted(database.manifests_for_captures(training_captures)),
            )
            return result
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
        if row["kind"] == "physical_io":
            payload["physical_measurements"] = read_physical_measurements(payload, row["deployment"])
        elif row["kind"] == "control_costs":
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


def physical_evidence(physical: dict[str, Any], deployment: dict[str, Any]) -> dict[str, Any]:
    """Identify a physical measurement collection by its explicit original inputs."""

    sources = sorted(physical["measurement_sources"])
    return dict(
        capture="physical:" + ",".join(sources),
        kind="physical_io",
        manifest=sources[0],
        deployment=deployment,
        payload=physical,
    )


def control_observations(evidence: dict[str, Any]) -> list[OperationObservation]:
    """Admit exported primitive measurements, not parameters or sample topology.

    Each evidence row describes one original capture. Multiple documents from
    that capture may repeat a primitive; its source position deduplicates it.
    CPU correction belongs to the document's own capture, never the base.
    """

    rows: list[OperationObservation] = []

    for _, value in control_entries(evidence["payload"].get("control_measurements", {})):
        observations = value.get("operator_observations", [])
        if not observations:
            continue

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

    return rows


def _with_measurement_sources(value: dict[str, Any]) -> dict[str, Any]:
    """Embed source pairing during import without modifying the measured document."""

    if "measurement_manifests" in value:
        return value
    manifests = [value["source_manifest"]]
    if value.get("cost_basis") == "paired_cpu_service":
        correction = load_json(require_repo_path(value["cpu_service_file"]))
        manifests.append(correction["light_manifest"])
    return dict(value, measurement_manifests=manifests)


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
            return _with_measurement_sources(value)

        return {key: embed(child) for key, child in value.items()}

    return {key: embed(value) for key, value in collection.items()}


def control_evidence(
    measurements: Sequence[ControlMeasurement], deployment: dict[str, Any], database: CostDatabase
) -> list[dict[str, Any]]:
    """Declare explicit control documents under their actual source capture.

    A group configuration and a stringified path collection are not capture
    identities. Selection paths are import inputs, not a second stored copy of
    operator measurements. The database deduplicates repeated source positions.
    """

    evidence = []
    for measurement in measurements:
        source = measurement.source
        manifest = str(repo_relative_path(source.manifest_path))
        collection: dict[str, Any] = {}
        selected = collection
        for key in measurement.selection[:-1]:
            selected = selected.setdefault(key, {})
        selected[measurement.selection[-1]] = _with_measurement_sources(measurement.document)
        evidence.append(
            dict(
                capture=database.capture_identity(manifest),
                manifest=manifest,
                deployment=deployment,
                kind="control_costs",
                payload={"control_measurements": collection},
            )
        )

    return evidence
