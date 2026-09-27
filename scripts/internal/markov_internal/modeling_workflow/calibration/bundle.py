"""Persist the compact physical calibration consumed by one-base fitting."""

from __future__ import annotations

from pathlib import Path

from ...common.io import write_json
from ...common.paths import repo_relative_path
from .aggregation import (
    build_prefetch_service_model,
    _new_operation_points,
    _existing_key_points,
)


def complete_service_bundle(
    base: dict, base_path: Path, observations: dict, output_dir: Path, wall_seconds: float, *, service: str
) -> dict[str, Path]:
    """Publish measured stages after capture admission, keeping other services unchanged."""

    output_path, observations_path = output_dir / "calibration_report.json", output_dir / "physical_observations.json"
    write_json(
        observations_path,
        dict(
            **observations,
            capture_wall_seconds=wall_seconds,
            reused_physical_report=str(repo_relative_path(base_path)),
            target_workload_trace_used=False,
            target_e2e_used=False,
        ),
    )
    services = dict(base["service_models"])
    if service == "prefetch":
        services["prefetch"] = build_prefetch_service_model(observations["prefetch_service_observations"])
        scope = "warm file allocation/read/copy/publication/return without inference"
    else:
        field, estimate = (
            ("existing_key_bandwidth_points", _existing_key_points)
            if service == "existing_write"
            else ("new_operation_points", _new_operation_points)
        )
        services["write_host_to_storage"] = {
            **services.get("write_host_to_storage", {"direction": "host_to_storage"}),
            field: estimate(observations["host_storage"]["selected_points"]),
        }
        scope = (
            "runtime materialize-then-batch-set of "
            + ("existing keys" if service == "existing_write" else "new keys")
            + " without inference"
        )

    _write_supplement(base, base_path, output_path, observations_path, services, service, scope)
    return dict(report_path=output_path, observations_path=observations_path)


def complete_dma_bundle(base: dict, base_path: Path, dma_path: Path, runtime: dict) -> Path:
    """Fill missing DMA curves without replacing previously admitted service costs."""

    services = dict(base["service_models"])
    for family, measured in runtime.items():
        if not services.get(family, {}).get("page_bandwidth_points"):
            services[family] = measured

    output = dma_path.parent / "calibration_report.json"
    _write_supplement(
        base, base_path, output, dma_path, services, "runtime_dma", "kernel_ascend/page_first_direct/pinned"
    )
    return output


def _write_supplement(
    base: dict, base_path: Path, output: Path, measurement: Path, services: dict, service: str, scope: str
) -> None:
    report = {
        **base,
        "service_models": services,
        "measurement_sources": list(
            dict.fromkeys(
                [
                    *base.get("measurement_sources", []),
                    str(repo_relative_path(base_path)),
                    str(repo_relative_path(measurement)),
                ]
            )
        ),
        "measurement_scope": {
            **base.get("measurement_scope", {}),
            service: scope,
        },
        "target_workload_trace_used": False,
        "target_e2e_used": False,
    }
    write_json(output, report)
