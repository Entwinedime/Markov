"""Explicit HiCache model geometry and I/O cost contract."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.paths import require_repo_path
from .io_model_contract import MAX_U64, OPERATION_KINDS, positive_u64
from .io_model_validation import (
    required_control_models,
    required_planning_rates,
    required_resource_lanes,
    required_service_models,
)


@dataclass(frozen=True)
class HiCacheIoModel:
    """Validated numerical fields for the workflow-wide HiCache cost model."""

    source_path: Path
    kv_bytes_per_token_per_rank: int
    planning_rates: tuple[tuple[str, int], ...]
    service_models: tuple[tuple[str, dict[str, Any]], ...]
    control_models: tuple[tuple[str, dict[str, Any]], ...]
    resource_lanes: tuple[tuple[str, str], ...]
    fields: dict[str, Any]

    @classmethod
    def load(cls, path: Path) -> HiCacheIoModel:
        """Load the one canonical I/O-model contract without conversion."""

        resolved = require_repo_path(path)
        if not resolved.is_file():
            raise FileNotFoundError(f"missing HiCache I/O model: {resolved}")
        raw = load_json(resolved)
        if not isinstance(raw, dict):
            raise TypeError(f"HiCache I/O model must be a JSON object: {resolved}")
        return cls.from_raw(resolved, raw)

    @classmethod
    def from_raw(cls, path: Path, raw: dict[str, Any]) -> HiCacheIoModel:
        service_models = required_service_models(raw.get("service_models"))
        control_models = required_control_models(raw.get("control_models"))
        resource_lanes = required_resource_lanes(raw.get("resource_lanes"))
        planning_rates = required_planning_rates(raw.get("planning_rates"))
        fields = {
            "kv_bytes_per_token_per_rank": positive_u64(
                raw.get("kv_bytes_per_token_per_rank"),
                "kv_bytes_per_token_per_rank",
            ),
            "planning_rates": planning_rates,
            "service_models": service_models,
            "control_models": control_models,
            "resource_lanes": resource_lanes,
        }
        return cls(
            source_path=path,
            kv_bytes_per_token_per_rank=fields["kv_bytes_per_token_per_rank"],
            planning_rates=tuple(planning_rates.items()),
            service_models=tuple((kind, service_models[kind]) for kind in OPERATION_KINDS),
            control_models=tuple((kind, control_models[kind]) for kind in OPERATION_KINDS),
            resource_lanes=tuple(resource_lanes.items()),
            fields=fields,
        )

    def kv_bytes_per_page(self, page_size: int) -> int:
        if page_size <= 0:
            raise ValueError("HiCache I/O model requires a positive target page_size")
        if page_size > MAX_U64 // self.kv_bytes_per_token_per_rank:
            raise OverflowError("target page_size * kv_bytes_per_token_per_rank exceeds uint64")
        return page_size * self.kv_bytes_per_token_per_rank

    def narrow_config(self, page_size: int) -> dict[str, Any]:
        service_models = dict(self.service_models)
        planning_rates = dict(self.planning_rates)
        page_bytes = self.kv_bytes_per_page(page_size)
        return {
            "kv_bytes_per_page": page_bytes,
            "io_planning": {
                "device_host_bandwidth_bytes_per_sec": planning_rates["device_host_bytes_per_sec"],
                "host_storage_bandwidth_bytes_per_sec": planning_rates["host_storage_bytes_per_sec"],
            },
            "io_cost": {
                "service_models": service_models,
                "control_models": dict(self.control_models),
                "resource_lanes": dict(self.resource_lanes),
            },
        }
