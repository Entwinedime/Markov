"""Project offline I/O and operator models into target execution inputs."""

from __future__ import annotations

from dataclasses import dataclass
import math
from pathlib import Path
from typing import Any

from ...common.io import load_json
from ...common.paths import require_repo_path
from .io_contract import MAX_U64, positive_u64
from .io_validation import required_resource_lanes, required_service_models


@dataclass(frozen=True)
class HiCacheIoModel:
    """Validated numerical fields for the workflow-wide HiCache cost model."""

    fields: dict[str, Any]

    @classmethod
    def load(cls, path: Path) -> HiCacheIoModel:
        """Load and validate the group-wide numerical model at its input boundary."""

        resolved = require_repo_path(path)
        raw = load_json(resolved)
        if not isinstance(raw, dict):
            raise TypeError(f"HiCache model must be a JSON object: {resolved}")
        return cls.from_raw(raw)

    @classmethod
    def from_raw(cls, raw: dict[str, Any]) -> HiCacheIoModel:
        if raw.get("control_models") or raw.get("phase_cost") is not None or "empty_write_check_us" in raw:
            raise ValueError(
                "Execution requires operator models, not aggregate phase/control costs or an empty-check override"
            )
        service_models = required_service_models(raw.get("service_models"))
        resource_lanes = required_resource_lanes(raw.get("resource_lanes"))
        fields = {
            "storage_batch_pages": positive_u64(raw.get("storage_batch_pages"), "storage_batch_pages"),
            "kv_bytes_per_token_per_rank": positive_u64(
                raw.get("kv_bytes_per_token_per_rank"),
                "kv_bytes_per_token_per_rank",
            ),
            "service_models": service_models,
            "resource_lanes": resource_lanes,
        }
        if "operator_models" in raw:
            fields["operator_models"] = raw["operator_models"]
        return cls(fields=fields)

    def narrow_config(self, page_size: int) -> dict[str, Any]:
        """Project a target into execution fields; nested coefficients remain read-only."""

        page_bytes = page_size * self.fields["kv_bytes_per_token_per_rank"]
        if page_bytes > MAX_U64:
            raise OverflowError("target page_size * kv_bytes_per_token_per_rank exceeds uint64")

        service_models = {}
        for kind, values in self.fields["service_models"].items():
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
                "storage_batch_pages": self.fields["storage_batch_pages"],
                "service_models": service_models,
                "resource_lanes": self.fields["resource_lanes"],
            },
        }
        if "operator_models" in self.fields:
            result["operator_models"] = self.fields["operator_models"]
        return result


def interpolate_cost_curve(
    points: list[dict[str, Any]], coordinate: float, coordinate_field: str, value_field: str, *, log_value: bool = True
) -> float:
    """Shared model-build/prediction curve; clamp endpoints on a log coordinate.

    Positive rates/scales use log values; setup costs use linear values, including zero.
    Callers admit measurement units and domains before constructing these curves.
    """
    ordered = sorted(points, key=lambda point: float(point[coordinate_field]))
    if coordinate <= float(ordered[0][coordinate_field]):
        return float(ordered[0][value_field])
    if coordinate >= float(ordered[-1][coordinate_field]):
        return float(ordered[-1][value_field])
    for left, right in zip(ordered, ordered[1:]):
        if coordinate > float(right[coordinate_field]):
            continue
        position = math.log(coordinate / float(left[coordinate_field])) / math.log(
            float(right[coordinate_field]) / float(left[coordinate_field])
        )
        left_value, right_value = float(left[value_field]), float(right[value_field])
        if log_value:
            return math.exp(math.log(left_value) + position * math.log(right_value / left_value))
        return left_value + position * (right_value - left_value)
    raise RuntimeError("cost curve interpolation failed")
