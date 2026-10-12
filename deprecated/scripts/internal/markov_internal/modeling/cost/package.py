"""Independent control measurement import and same-source CPU correction inputs."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ...common.io import load_json
from ...common.paths import repo_relative_path, require_repo_path
from ..source_dag.profiles import discover_profile_runs
from .database import OperationObservation, merge_control_measurements


def cpu_service_inputs(paths: list[Path], sources: tuple[Path, ...]) -> dict[Path, Path]:
    """Bind explicit service files to their own selected captures, never targets."""

    inputs = {}
    for path in paths:
        path = require_repo_path(path)
        source = require_repo_path(load_json(path)["source_manifest"]).resolve()
        if source not in sources:
            raise ValueError(f"CPU service input does not belong to a selected source: {path}")
        if source in inputs:
            raise ValueError(f"Multiple CPU service inputs for the same source: {source}")
        inputs[source] = path
    return inputs


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
