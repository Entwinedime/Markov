"""Build shared HiCache costs from base evidence, supplemented only for missing costs."""

from __future__ import annotations

import math
from pathlib import Path
from statistics import median
from typing import TYPE_CHECKING, Any

from ..common.io import write_json
from .io_model import HiCacheIoModel, interpolate_cost_curve
from .io_model_contract import (
    OPERATION_KINDS,
    KIND_DIRECTIONS,
    io_observation_ready,
    positive_u64,
    positive_finite_number,
    nonnegative_finite_number,
)
from .io_model_validation import _points, required_resource_lanes, required_prefetch_stages
from .phase_calibration import build_phase_cost, MissingPhaseEvidence

if TYPE_CHECKING:
    from .group import GroupRequest


class MissingCostEvidence(ValueError):
    """A supported cost form lacks measurements that identify its parameters."""


def _existing_bandwidth(points: list[dict[str, Any]], page_bytes: float, operation_pages: float) -> float:
    by_page: dict[int, list[dict[str, Any]]] = {}
    for point in points:
        by_page.setdefault(int(point["page_bytes"]), []).append(point)
    curve = [
        {
            "page_bytes": calibrated_page,
            "bandwidth_bytes_per_sec": interpolate_cost_curve(
                values, operation_pages, "operation_pages", "bandwidth_bytes_per_sec"
            ),
        }
        for calibrated_page, values in sorted(by_page.items())
    ]
    return interpolate_cost_curve(curve, page_bytes, "page_bytes", "bandwidth_bytes_per_sec")


def _existing_write_projection(observed: dict[str, Any], service: dict[str, Any], page_bytes: float) -> float:
    existing = 0.0
    for batch in observed["storage_service_batches"]:
        existing_pages = int(batch["storage_existing_page_count"])
        if existing_pages:
            bandwidth = _existing_bandwidth(service["existing_key_bandwidth_points"], page_bytes, batch["page_count"])
            existing += existing_pages * page_bytes * 1e6 / bandwidth
    return existing


def observed_service_work(observed: dict[str, Any], geometry: int) -> dict[str, Any] | None:
    """Extract measured work without requiring or estimating physical costs."""

    kind = observed.get("kind")
    if kind not in OPERATION_KINDS or not io_observation_ready(observed) or not observed["service_observed"]:
        return None
    pages = int(observed["service_page_count"])
    service_us = float(observed.get("service_us") or 0.0)
    page_size = int(observed.get("page_size") or 0)
    if pages <= 0 or service_us <= 0 or page_size <= 0:
        return None
    page_bytes = page_size * geometry
    byte_count = pages * page_bytes
    existing_pages = new_pages = 0
    copied_pages = None
    if kind == "prefetch":
        calls = len(observed["storage_service_batches"])
        if calls == 0:
            return None
        batches = observed["storage_service_batches"]
        if sum(int(batch["page_count"]) for batch in batches) != pages or any(
            batch.get("copied_page_count") is None
            or not 0 < int(batch["copied_page_count"]) <= int(batch["page_count"])
            for batch in batches
        ):
            return None
        copied_pages = sum(int(batch["copied_page_count"]) for batch in batches)
    elif kind == "write_host_to_storage":
        if not observed.get("storage_residency_observed") or not observed.get("storage_service_batches"):
            return None
        batches = observed["storage_service_batches"]
        existing_pages = sum(int(batch["storage_existing_page_count"]) for batch in batches)
        new_pages = sum(int(batch["storage_new_page_count"]) for batch in batches)

    if kind in {"prefetch", "write_host_to_storage"}:
        service_call_bytes = [
            int(batch["page_count"]) * page_bytes for batch in observed.get("storage_service_batches", [])
        ]
    else:
        operation_count = int(observed.get("operation_count") or 0)
        service_call_bytes = [byte_count / operation_count] * operation_count if operation_count > 0 else []
    return {
        "family": kind,
        "source_manifest": None,
        "observed_service_us": service_us,
        "storage_existing_page_count": existing_pages,
        "storage_new_page_count": new_pages,
        "page_size": page_size,
        "page_count": pages,
        "copied_page_count": copied_pages,
        "byte_count": byte_count,
        "storage_new_batch_pages": [
            int(batch.get("storage_new_page_count") or 0)
            for batch in observed.get("storage_service_batches", [])
            if int(batch.get("storage_new_page_count") or 0) > 0
        ],
        "service_call_bytes": service_call_bytes,
    }


def _physical_service(observed: dict[str, Any], services: dict[str, Any], geometry: int) -> dict[str, Any] | None:
    row = observed_service_work(observed, geometry)
    if row is None:
        return None

    kind = row["family"]
    # Pure-new writes identify their own setup and bandwidth. They do not use
    # the existing-key curve, even when that independent measurement is absent.
    if kind == "write_host_to_storage" and row["storage_existing_page_count"] == 0:
        row["physical_service_us"] = 0.0
        return row

    model = services.get(kind, {})
    pages, byte_count = row["page_count"], row["byte_count"]
    page_bytes = row["page_size"] * geometry
    if kind == "prefetch":
        if "stages" not in model:
            return None
        stages = model["stages"]
        physical_us = (
            (stages["before_copy_us_per_page"] + stages["before_copy_us_per_byte"] * page_bytes) * pages
            + (stages["copy_publish_us_per_page"] + stages["copy_publish_us_per_byte"] * page_bytes)
            * row["copied_page_count"]
            + stages["return_us_per_operation"] * len(row["service_call_bytes"])
            + stages["return_us_per_page"] * pages
        )
    elif kind in {"load", "write_device_to_host"}:
        if not model.get("page_bandwidth_points"):
            return row  # Keep raw DMA work available for base-only identification.
        bandwidth = interpolate_cost_curve(
            model["page_bandwidth_points"], page_bytes, "page_bytes", "bandwidth_bytes_per_sec"
        )
        setup = interpolate_cost_curve(
            model["page_bandwidth_points"], page_bytes, "page_bytes", "setup_us_per_operation", log_value=False
        )
        physical_us = setup * observed["operation_count"] + byte_count * 1e6 / bandwidth
    else:
        if not model.get("existing_key_bandwidth_points"):
            return None
        physical_us = _existing_write_projection(observed, model, page_bytes)

    if physical_us <= 0 and row["storage_new_page_count"] == 0:
        return None

    row["physical_service_us"] = physical_us
    return row


def service_observation_rows(physical: dict[str, Any], captures: list[dict[str, Any]]) -> list[dict[str, Any]]:
    services = physical["service_models"]
    geometry = int(physical["kv_geometry"]["kv_bytes_per_token_per_rank"])
    rows = []
    for capture in captures:
        for observed in capture["source_io_observations"]["observations"]:
            row = _physical_service(observed, services, geometry)
            if row is not None:
                row["source_manifest"] = capture["source_manifest"]
                row["role"] = capture["role"]
                rows.append(row)
    return rows


def _runtime_scale_curve(
    rows: list[dict[str, Any]], component: str, geometry: int
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Estimate a multiplier from rows already selected for one service/state."""
    by_capture: dict[str, list[dict[str, Any]]] = {}
    for row in rows:
        by_capture.setdefault(row["source_manifest"], []).append(row)
    details = []
    for source, values in sorted(by_capture.items()):
        observed = math.fsum(row["observed_service_us"] for row in values)
        physical = math.fsum(row["physical_service_us"] for row in values)
        if observed <= 0 or physical <= 0:
            continue
        ratio = observed / physical
        details.append(
            {
                "source_manifest": source,
                "role": values[0]["role"],
                "page_size": values[0]["page_size"],
                "operation_count": len(values),
                "observed_service_us": observed,
                "physical_service_us": physical,
                "runtime_scale": ratio,
            }
        )
    if not details:
        raise MissingCostEvidence(f"no positive measurements identify {component} service")
    points = []
    anchors_by_page: dict[int, list[dict[str, Any]]] = {}
    for row in details:
        anchors_by_page.setdefault(row["page_size"], []).append(row)
    for page_size, page_rows in sorted(anchors_by_page.items()):
        points.append(
            {
                "page_bytes": page_size * geometry,
                "runtime_scale": median(row["runtime_scale"] for row in page_rows),
                "source_manifests": [row["source_manifest"] for row in page_rows],
                "repeat_count": len(page_rows),
            }
        )
    observed_total = math.fsum(row["observed_service_us"] for row in rows)
    physical_total = math.fsum(row["physical_service_us"] for row in rows)
    model_points = [{"page_bytes": row["page_bytes"], "runtime_scale": row["runtime_scale"]} for row in points]
    return model_points, {
        "formula": (
            "per observed page size: median capture-level observed/physical ratio; "
            "log interpolation only when multiple page anchors exist"
        ),
        "unit": "dimensionless",
        "capture_count": len(details),
        "page_anchor_count": len(points),
        "operation_count": len(rows),
        "observed_service_us": observed_total,
        "physical_service_us": physical_total,
        "per_capture": details,
        "points": points,
    }


def _call_cost_curve(rows: list[dict[str, Any]], geometry: int) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Identify T = calls * setup + bytes / bandwidth for one service family.

    Rows belong to one family; storage rows must contain pure-new writes.
    Normalize by calls before solving the two observed size anchors. Negative
    setup or nonpositive byte cost is insufficient evidence, not a value to clip.
    """

    by_page: dict[int, list[dict[str, Any]]] = {}
    for row in rows:
        by_page.setdefault(row["page_size"], []).append(row)
    fits, rejected = [], []
    for page, values in sorted(by_page.items()):
        samples: dict[float, list[dict]] = {}
        for row in values:
            calls = len(
                row["storage_new_batch_pages"]
                if row["family"] == "write_host_to_storage"
                else row["service_call_bytes"]
            )
            if calls == 0:
                continue
            samples.setdefault(row["byte_count"] / calls, []).append(
                dict(
                    source=row["source_manifest"],
                    role=row.get("role", "calibration"),
                    duration=row["observed_service_us"] / calls,
                )
            )
        anchors = []
        for size, observations in sorted(samples.items()):
            chosen = [row for row in observations if row["role"] == "base"] or observations
            per_source = {}
            for row in chosen:
                per_source.setdefault(row["source"], []).append(row["duration"])
            anchors.append(
                dict(
                    bytes_per_call=size,
                    duration_us_per_call=median(median(times) for times in per_source.values()),
                    sample_count=len(chosen),
                    source_manifests=sorted(per_source),
                )
            )
        if len(anchors) < 2:
            rejected.append(dict(page_size=page, reason="two distinct bytes-per-call sizes required"))
            continue
        short, long = anchors[0], anchors[-1]
        slope = (long["duration_us_per_call"] - short["duration_us_per_call"]) / (
            long["bytes_per_call"] - short["bytes_per_call"]
        )
        setup = short["duration_us_per_call"] - slope * short["bytes_per_call"]
        if setup < 0 or slope <= 0:
            rejected.append(
                dict(page_size=page, reason="measurements do not identify non-negative setup and positive bandwidth")
            )
            continue
        fits.append(
            {
                "page_size": page,
                "setup_us_per_operation": setup,
                "bandwidth_bytes_per_sec": 1_000_000.0 / slope,
                "fit_anchors": [short, long],
                "observed_anchors": anchors,
            }
        )
    points = [
        dict(
            page_bytes=row["page_size"] * geometry,
            setup_us_per_operation=row["setup_us_per_operation"],
            bandwidth_bytes_per_sec=row["bandwidth_bytes_per_sec"],
        )
        for row in fits
    ]
    if not points:
        raise MissingCostEvidence("bytes-per-call measurements cannot identify setup and bandwidth: " + str(rejected))
    return points, {
        "formula": "per endpoint: operation duration = service_calls * setup_per_call + bytes / bandwidth",
        "parameter_method": "normalize by service-call count; prefer base per size; median per capture then per size; solve two anchors",
        "units": {"setup_us_per_operation": "microseconds", "bandwidth_bytes_per_sec": "bytes/second"},
        "per_page": fits,
        "unidentified_pages": rejected,
        "points": points,
    }


def _profile_evidence(
    rows: list[dict], component: str, geometry: int, *, call_cost: bool
) -> tuple[list[dict] | None, dict]:
    """Choose base before shared observations and retain the failed base reason."""

    base_gap = None
    base_rows = [row for row in rows if row["role"] == "base"]
    for origin, candidates in (("base", base_rows), ("base_with_supplement", rows)):
        try:
            curve, detail = (
                _call_cost_curve(candidates, geometry)
                if call_cost
                else _runtime_scale_curve(candidates, component, geometry)
            )
        except MissingCostEvidence as error:
            if origin == "base":
                base_gap = str(error)
                if len(base_rows) == len(rows):
                    return None, dict(base_gap=base_gap, profile_gap=base_gap)
                continue
            return None, dict(base_gap=base_gap, profile_gap=str(error))

        if call_cost:
            manifests = {
                name
                for page in detail["per_page"]
                for anchor in page["fit_anchors"]
                for name in anchor["source_manifests"]
            }
        else:
            manifests = {item["source_manifest"] for item in detail["per_capture"]}
        return curve, dict(detail, evidence_origin=origin, base_gap=base_gap, source_manifests=sorted(manifests))


def service_cost_evidence(physical: dict | None, captures: list[dict]) -> dict:
    """The same evidence decision drives service readiness and model construction.

    Captures have unique manifests and admitted roles from prepare_model.
    A base-derived multiplier may project sizes through the physical model. A
    second calibrated page endpoint is not a prerequisite for that projection.
    This function does not certify CPU overhead or an unobserved backend.
    """
    if physical is None:
        return dict(
            status="data_limitation",
            missing=[dict(component="physical", reason="platform_model_not_available")],
            sources={},
            models={},
            rows=[],
        )
    services = dict(physical["service_models"])
    stages = services.get("prefetch", {}).get("stages")
    missing = []
    if stages is None:
        missing.append(
            dict(
                component="physical/prefetch_stages",
                reason="read_copy_return_measurements_missing; total service time cannot identify stage costs",
            )
        )
    else:
        required_prefetch_stages(stages)
    rows = service_observation_rows(physical, captures)
    geometry = int(physical["kv_geometry"]["kv_bytes_per_token_per_rank"])
    components = [(kind, kind, None) for kind in OPERATION_KINDS[:-1]] + [
        ("write_host_to_storage_existing", "write_host_to_storage", "existing"),
        ("write_host_to_storage_new", "write_host_to_storage", "new"),
    ]
    scales, sources, selected_rows = {}, {}, []
    new_write = []
    for component, family, state in components:
        if family == "prefetch" and stages is None:
            continue
        if family in {"load", "write_device_to_host"}:
            dma_rows = [row for row in rows if row["family"] == family]
            curve, detail = _profile_evidence(dma_rows, component, geometry, call_cost=True)
            if curve is not None:
                services[family] = dict(direction=KIND_DIRECTIONS[family], page_bandwidth_points=curve)
                scales[component] = [dict(page_bytes=point["page_bytes"], runtime_scale=1.0) for point in curve]
                sources[component] = dict(
                    detail,
                    parameter_basis="measured call setup and byte cost; no independent DMA curve or multiplier",
                    extrapolation="endpoint coefficients outside observed pages and call sizes are unverified",
                    timing_scope="observed DMA service; no residual CPU or target correction",
                )
                pages = {page["page_size"] for page in detail["per_page"]}
                selected_rows.extend(
                    row
                    for row in dma_rows
                    if row["source_manifest"] in detail["source_manifests"] and row["page_size"] in pages
                )
                continue
        if family != "prefetch" and state != "new":
            parameter = "existing_key_bandwidth_points" if state == "existing" else "page_bandwidth_points"
            if not services.get(family, {}).get(parameter):
                missing.append(dict(component="physical/" + component, reason=parameter + "_missing"))
                continue

        eligible = [
            row
            for row in rows
            if row["family"] == family
            and (
                state is None
                or (
                    row[f"storage_{state}_page_count"] > 0
                    and row[f"storage_{'existing' if state == 'new' else 'new'}_page_count"] == 0
                )
            )
        ]
        curve, detail = _profile_evidence(eligible, component, geometry, call_cost=state == "new")
        if curve is None:
            independent = _independent_service_curve(physical, family, state)
            if independent is None:
                missing.append(
                    dict(component="service/" + component, reason=detail["profile_gap"], base_reason=detail["base_gap"])
                )
                continue
            if state == "new":
                new_write = independent
            else:
                scales[component] = independent
            sources[component] = dict(
                detail,
                evidence_origin="independent_physical",
                measurement_sources=list(physical["measurement_sources"]),
                measurement_scope=physical.get("measurement_scope", {}),
                points=independent,
                formula="independently measured service formula; no runtime multiplier estimated",
                extrapolation="declared service formula outside measured work; inference contention is unverified",
                timing_scope="independent I/O service; no target or runtime-residual fitting",
            )
            continue

        if state == "new":
            new_write = curve
            pages = {page["page_size"] for page in detail["per_page"]}
        else:
            scales[component] = curve
            pages = {item["page_size"] for item in detail["per_capture"]}
        sources[component] = dict(
            detail,
            extrapolation="page-endpoint coefficients held constant outside measured pages; work scales through the declared service formula",
            timing_scope="observed I/O service; unmeasured profiler/scheduling perturbation is not claimed removed",
        )
        selected_rows.extend(
            row
            for row in eligible
            if row["source_manifest"] in detail["source_manifests"] and row["page_size"] in pages
        )
    available = set(scales) & set(OPERATION_KINDS)
    if new_write or "write_host_to_storage_existing" in scales:
        available.add("write_host_to_storage")
    models = {
        kind: _service_model(kind, services.get(kind, {}), scales, new_write)
        for kind in OPERATION_KINDS
        if kind in available
    }
    return dict(
        status="data_limitation" if missing else "ready",
        missing=missing,
        sources=sources,
        models=models,
        rows=selected_rows,
    )


def _independent_service_curve(physical: dict, family: str, state: str | None) -> list[dict] | None:
    """Use declared primitive measurements only after workload evidence is exhausted.

    Unit scales express the absence of a runtime correction, not a measured
    workload response. In particular, they must not create observation coverage.
    """

    if not physical.get("measurement_sources"):
        return None

    service = physical["service_models"].get(family, {})
    if state == "new":
        points = service.get("new_operation_points")
        if not points:
            return None
        return _points(
            points,
            "physical new-write parameters",
            dict(
                page_bytes=positive_u64,
                setup_us_per_operation=nonnegative_finite_number,
                bandwidth_bytes_per_sec=positive_finite_number,
            ),
            ("page_bytes",),
        )

    if family == "prefetch":
        # Prefetch stages are validated before selection. This reference point
        # encodes an identity multiplier at every page size, not a sampled page.
        page_bytes = [int(physical["kv_geometry"]["kv_bytes_per_token_per_rank"])]
    else:
        field = "existing_key_bandwidth_points" if state == "existing" else "page_bandwidth_points"
        page_bytes = sorted({point["page_bytes"] for point in service[field]})
        if not page_bytes:
            return None
    return [dict(page_bytes=page, runtime_scale=1.0) for page in page_bytes]


def _service_model(
    family: str, physical: dict[str, Any], scales: dict[str, list[dict[str, Any]]], new_write: list[dict[str, Any]]
) -> dict[str, Any]:
    fields = {"direction": physical.get("direction", KIND_DIRECTIONS[family])}
    if family == "prefetch":
        fields.update(
            runtime_scale_points=scales[family],
            stages=dict(physical["stages"]),
        )
    elif family in {"load", "write_device_to_host"}:
        fields["page_bandwidth_points"] = physical["page_bandwidth_points"]
        fields["runtime_scale_points"] = scales[family]
    else:
        if new_write:
            fields["new_operation_points"] = new_write
        if "write_host_to_storage_existing" in scales:
            fields.update(
                existing_key_bandwidth_points=physical["existing_key_bandwidth_points"],
                existing_runtime_scale_points=scales["write_host_to_storage_existing"],
            )
    return fields


def _io_coverage(rows: list[dict[str, Any]]) -> dict[str, Any]:
    """Describe observed calibration support without changing model behavior."""

    page_bytes = sorted({int(row["byte_count"] / row["page_count"]) for row in rows if row["page_count"] > 0})
    calls: dict[str, dict[str, Any]] = {}
    for family in OPERATION_KINDS:
        values = [
            float(value) for row in rows if row["family"] == family for value in row["service_call_bytes"] if value > 0
        ]
        if values:
            calls[family] = {
                "min": min(values),
                "max": max(values),
                "sample_count": len(values),
            }
    return {
        "source": "selected_base_and_supplement_observations",
        "page_bytes": {"min": min(page_bytes), "max": max(page_bytes), "anchors": page_bytes} if page_bytes else None,
        "service_call_bytes": calls,
        "outside_domain_behavior": "page endpoint clamp; service-call size remains a physical-model projection and is reported unverified",
    }


def prepare_model(group: GroupRequest, captures: list[dict[str, Any]], *, output: Path | None = None) -> dict[str, Any]:
    """Select costs once and optionally publish a model, returning its input report.

    Missing I/O services remain absent; execution reports a gap if it uses them.
    Base, geometry and phase gaps prevent publication. Execution control costs
    come from operation observations, not the historical static scalar model.
    """

    # Primitive acquisition imports group definitions, so keep this dependency local.
    from .calibration.eviction_cpu import group_locked_candidate_cost

    if any(capture["role"] not in {"base", "calibration"} for capture in captures):
        raise ValueError("Cost evidence must be base or independent calibration, never target observations")
    if len({capture["source_manifest"] for capture in captures}) != len(captures):
        raise ValueError("Provide each observation manifest once; repeated captures need distinct manifests")

    bases = [capture for capture in captures if capture["role"] == "base"]
    calibration = [capture for capture in captures if capture["role"] == "calibration"]
    physical = group.physical
    evidence = service_cost_evidence(physical, captures)
    missing = list(evidence["missing"]) if physical is None else []
    if len(bases) != len(group.sources):
        missing.append({"component": "base", "reason": "base_observations_incomplete"})
    phase, phase_summary = None, {}
    try:
        phase, phase_summary = build_phase_cost(captures, int(group.sources[0].hicache_config["page_size"]))
    except MissingPhaseEvidence as error:
        missing.append({"component": "phase", "reason": str(error)})

    report = {
        "status": "ready" if not missing else "data_limitation",
        "missing": missing,
        "service_gaps": evidence["missing"] if physical is not None else [],
        "base_profile_count": len(bases),
        "calibration_profile_count": len(calibration),
        "service_observation_count": {
            kind: sum(row["family"] == kind for row in evidence["rows"]) for kind in OPERATION_KINDS
        },
        "service_evidence": evidence["sources"],
        "execution_control_scope": "prefetch programs requested on execution; other CPU and phase prerequisites remain",
        "phase_evidence": phase_summary.get("parameter_sources", {}),
        "target_configurations": [target.label for target in group.targets],
    }
    if output is None:
        return report
    if missing:
        write_json(output / "model_build_summary.json", {"status": "needs_calibration_data", "model_inputs": report})
        return report

    geometry = int(physical["kv_geometry"]["kv_bytes_per_token_per_rank"])
    service_sources = evidence["sources"]
    model = {
        "storage_batch_pages": physical["storage_batch_pages"],
        "kv_bytes_per_token_per_rank": geometry,
        "service_models": evidence["models"],
        "control_models": {},
        "resource_lanes": required_resource_lanes(physical.get("resource_lanes")),
        "phase_cost": phase,
    }
    if group.control_sources:
        model["control_calibrations"] = group.raw["control_calibrations"]
    summary = {
        "status": "ready",
        "service_gaps": evidence["missing"],
        "model_form": (
            "base-first service evidence; independent primitive costs fill gaps without an inferred runtime multiplier"
        ),
        "parameter_sources": {
            "physical": "base deployment metadata and declared independent service measurements",
            "service_runtime_scales": service_sources,
            "phase": phase_summary["parameter_sources"],
        },
        "platform_measurement": {
            "description": physical.get("measurement_description"),
            "sources": list(physical.get("measurement_sources") or []),
            "scope": physical.get("measurement_scope") or {},
        },
        "observation_sources": {
            "base": [capture["source_manifest"] for capture in bases],
            "fixed_calibration": [capture["source_manifest"] for capture in calibration],
        },
        "phase": {key: value for key, value in phase_summary.items() if key != "parameter_sources"},
        "io_coverage": _io_coverage(evidence["rows"]),
    }
    summary["base_group"] = group.base_config
    summary["parameter_sources"]["execution_control"] = group.control_sources
    operation_costs, operation_evidence = group_locked_candidate_cost(group)
    if operation_costs:
        model.update(operation_costs)
        summary["parameter_sources"]["locked_candidate"] = operation_evidence
    model = HiCacheIoModel.from_raw(model).fields
    write_json(output / "hicache_io_model.json", model)
    write_json(output / "model_build_summary.json", summary)
    return report


if __name__ == "__main__":
    # The public build action shares preparation's observation admission path.
    from .observations import main

    raise SystemExit(main(build_model=True))
