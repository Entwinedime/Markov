"""Admit deployment, provenance and I/O coverage for one independent cost package."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ...common.paths import require_repo_path
from ..deployment import require_cost_environment
from .io_contract import positive_u64
from .io_validation import required_resource_lanes, required_service_models


@dataclass(frozen=True)
class HiCacheCostModels:
    """Deployment and independent measurement sources of one shared package.

    Admission resolves paths and validates the I/O domain once. Numeric model
    data stays in the shared package file, loaded by the native session. A partial
    package remains usable until an operation requires a missing response.
    Capture names are database provenance, not suite-local profiling run IDs;
    isolation uses the complete manifest lineage, including paired sources.
    """

    deployment: dict[str, Any]
    source_captures: tuple[str, ...]
    source_manifests: tuple[Path, ...]

    @classmethod
    def from_raw(cls, raw: dict[str, Any]) -> HiCacheCostModels:
        require_cost_environment(raw["deployment"])
        io = raw["hicache"]
        if io.get("control_models") or io.get("phase_cost") is not None or "empty_write_check_us" in io:
            raise ValueError(
                "Execution requires operator models, not aggregate phase/control costs or an empty-check override"
            )

        positive_u64(io["storage_batch_pages"], "storage_batch_pages")
        positive_u64(io["kv_bytes_per_token_per_rank"], "kv_bytes_per_token_per_rank")
        required_resource_lanes(io["resource_lanes"])
        required_service_models(io["service_models"])
        return cls(
            deployment=raw["deployment"],
            source_captures=tuple(raw["training_sources"]),
            source_manifests=tuple(require_repo_path(path).resolve() for path in raw["training_source_manifests"]),
        )
