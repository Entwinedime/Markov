"""Publish physical measurement sources; offline training owns all parameters."""

from __future__ import annotations

from pathlib import Path

from markov_internal.common.io import write_json
from markov_internal.common.paths import repo_relative_path


def complete_service_bundle(
    base: dict, base_path: Path, observations: dict, output_dir: Path, wall_seconds: float, *, service: str
) -> dict[str, Path]:
    """Publish raw stages and retained source references, without fitting costs."""

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
    if service == "prefetch":
        scope = "warm file allocation/read/copy/publication/return without inference"
    else:
        scope = (
            "runtime materialize-then-batch-set of "
            + ("existing keys" if service == "existing_write" else "new keys")
            + " without inference"
        )

    _write_supplement(base, base_path, output_path, observations_path, service, scope)
    return dict(report_path=output_path, observations_path=observations_path)


def complete_dma_bundle(base: dict, base_path: Path, dma_path: Path) -> Path:
    """Publish device measurements without constructing or replacing curves."""

    output = dma_path.parent / "calibration_report.json"
    _write_supplement(base, base_path, output, dma_path, "runtime_dma", "kernel_ascend/page_first_direct/pinned")
    return output


def _write_supplement(base: dict, base_path: Path, output: Path, measurement: Path, service: str, scope: str) -> None:
    report = {
        **{key: base[key] for key in ("kv_geometry", "resource_lanes", "storage_batch_pages")},
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
