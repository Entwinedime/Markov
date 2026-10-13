"""Admit one independent cost package and project its execution inputs."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ...common.paths import require_repo_path
from ..deployment import require_cost_environment
from .curves import interpolate_cost_curve
from .io_contract import MAX_U64, positive_u64
from .io_validation import required_resource_lanes, required_service_models


@dataclass(frozen=True)
class HiCacheCostModels:
    """One package's responses, deployment and independent measurement sources.

    Admission resolves paths and validates the I/O domain once. Nested model
    data is shared read-only across targets; execution cannot mutate coefficients
    or interpret these sources as traces to read. A partial package remains
    usable until an actual operation requires a missing response.
    Capture names are database provenance, not suite-local profiling run IDs;
    isolation uses the complete manifest lineage, including paired sources.
    """

    deployment: dict[str, Any]
    source_captures: tuple[str, ...]
    source_manifests: tuple[Path, ...]
    storage_batch_pages: int
    kv_bytes_per_token_per_rank: int
    resource_lanes: dict[str, str]
    service_models: dict[str, dict[str, Any]]
    operator_models: list[dict[str, Any]]

    @classmethod
    def from_raw(cls, raw: dict[str, Any]) -> HiCacheCostModels:
        require_cost_environment(raw["deployment"])
        io = raw["hicache"]
        if io.get("control_models") or io.get("phase_cost") is not None or "empty_write_check_us" in io:
            raise ValueError(
                "Execution requires operator models, not aggregate phase/control costs or an empty-check override"
            )
        return cls(
            deployment=raw["deployment"],
            source_captures=tuple(raw["training_sources"]),
            source_manifests=tuple(require_repo_path(path).resolve() for path in raw["training_source_manifests"]),
            storage_batch_pages=positive_u64(io["storage_batch_pages"], "storage_batch_pages"),
            kv_bytes_per_token_per_rank=positive_u64(
                io["kv_bytes_per_token_per_rank"],
                "kv_bytes_per_token_per_rank",
            ),
            resource_lanes=required_resource_lanes(io["resource_lanes"]),
            service_models=required_service_models(io["service_models"]),
            operator_models=raw["operator_models"],
        )

    def execution_costs(self, page_size: int) -> dict[str, Any]:
        """Project a target into execution fields; nested coefficients remain read-only."""

        page_bytes = page_size * self.kv_bytes_per_token_per_rank
        if page_bytes > MAX_U64:
            raise OverflowError("target page_size * kv_bytes_per_token_per_rank exceeds uint64")

        service_models = {}
        for kind, values in self.service_models.items():
            # Only top-level curve fields are replaced. Coefficients and point
            # tables are shared read-only, like operator models and resource lanes.
            narrowed = dict(values)
            if kind == "write_host_to_storage":
                if "existing_runtime_scale_points" in narrowed:
                    existing = narrowed.pop("existing_runtime_scale_points")
                    narrowed["existing_runtime_scale"] = interpolate_cost_curve(
                        existing, page_bytes, "page_bytes", "runtime_scale"
                    )
            else:
                points = narrowed.pop("runtime_scale_points")
                narrowed["runtime_scale"] = interpolate_cost_curve(points, page_bytes, "page_bytes", "runtime_scale")
            service_models[kind] = narrowed
        result = {
            "kv_bytes_per_page": page_bytes,
            "io_cost": {
                "storage_batch_pages": self.storage_batch_pages,
                "service_models": service_models,
                "resource_lanes": self.resource_lanes,
            },
            "operator_models": self.operator_models,
        }
        return result
