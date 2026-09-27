"""Explicit HiCache geometry and unified I/O/control plus phase cost contract."""

from __future__ import annotations

import copy
from dataclasses import dataclass
import math
from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.paths import require_repo_path
from .io_model_contract import MAX_U64, nonnegative_finite_number, positive_finite_number, positive_u64
from .control_calibrations import control_calibrations, select_control_calibrations
from .io_model_validation import (
    _points,
    _exact_object,
    required_control_models,
    required_resource_lanes,
    required_service_models,
)


@dataclass(frozen=True)
class HiCacheIoModel:
    """Validated numerical fields for the workflow-wide HiCache cost model."""

    fields: dict[str, Any]

    @classmethod
    def load(cls, path: Path) -> HiCacheIoModel:
        """Load the one canonical HiCache model contract without conversion."""

        resolved = require_repo_path(path)
        raw = load_json(resolved)
        if not isinstance(raw, dict):
            raise TypeError(f"HiCache model must be a JSON object: {resolved}")
        return cls.from_raw(raw)

    @classmethod
    def from_raw(cls, raw: dict[str, Any]) -> HiCacheIoModel:
        service_models = required_service_models(raw.get("service_models"))
        control_models = required_control_models(raw.get("control_models"))
        resource_lanes = required_resource_lanes(raw.get("resource_lanes"))
        phase_cost = _phase_cost(raw.get("phase_cost"))
        fields = {
            "storage_batch_pages": positive_u64(raw.get("storage_batch_pages"), "storage_batch_pages"),
            "kv_bytes_per_token_per_rank": positive_u64(
                raw.get("kv_bytes_per_token_per_rank"),
                "kv_bytes_per_token_per_rank",
            ),
            "service_models": service_models,
            "control_models": control_models,
            "resource_lanes": resource_lanes,
            "phase_cost": phase_cost,
        }
        for name in ("empty_write_check_us", "locked_candidate_us"):
            if name in raw:
                fields[name] = positive_finite_number(raw[name], name)
        if "locked_candidate_log2_heap_us" in raw:
            if "locked_candidate_us" not in fields:
                raise ValueError("Locked-candidate heap cost requires a fixed cost")
            fields["locked_candidate_log2_heap_us"] = nonnegative_finite_number(
                raw["locked_candidate_log2_heap_us"], "locked_candidate_log2_heap_us"
            )
        if "control_calibrations" in raw:
            fields["control_calibrations"] = control_calibrations(raw["control_calibrations"])
        return cls(fields=fields)

    def narrow_config(
        self, page_size: int, prefetch_policy: str, write_policy: str = "write_through"
    ) -> dict[str, Any]:
        """Project a validated target into C++ costs without changing this model."""

        page_bytes = page_size * self.fields["kv_bytes_per_token_per_rank"]
        if page_bytes > MAX_U64:
            raise OverflowError("target page_size * kv_bytes_per_token_per_rank exceeds uint64")

        service_models = {}
        for kind, values in self.fields["service_models"].items():
            narrowed = copy.deepcopy(values)
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
        controls = {kind: dict(values) for kind, values in self.fields["control_models"].items()}
        if controls:
            check = controls["prefetch"].pop("state_check_us_per_operation")
            if prefetch_policy != "best_effort":
                controls["prefetch"]["fixed_us_per_operation"] += check
        result = {
            "kv_bytes_per_page": page_bytes,
            "io_cost": {
                "storage_batch_pages": self.fields["storage_batch_pages"],
                "service_models": service_models,
                "control_models": controls,
                "resource_lanes": self.fields["resource_lanes"],
            },
            "phase_cost": self.fields["phase_cost"],
        }
        for name in ("empty_write_check_us", "locked_candidate_us", "locked_candidate_log2_heap_us"):
            if name in self.fields:
                result["io_cost"][name] = self.fields[name]
        result.update(
            select_control_calibrations(
                self.fields.get("control_calibrations", {}),
                page_size,
                prefetch_policy,
                write_policy,
            )
        )
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


_PHASE_CURVES = (
    "prefill_common_kernel",
    "prefill_collective",
)
_PHASE_COMPONENTS = (
    "prefill_prefix_attention",
    "decode_collective",
)
_PHASE_COEFFICIENTS = (
    "fixed_us",
    "per_new_token_us",
    "per_attention_token_pair_us",
    "per_context_token_us",
)
_PHASE_COVERAGE = (
    "min_new_tokens",
    "max_new_tokens",
    "min_context_tokens",
    "max_context_tokens",
    "min_attention_token_pairs",
    "max_attention_token_pairs",
    "min_decode_context_tokens",
    "max_decode_context_tokens",
    "base_page_size",
)


def _phase_cost(raw: Any) -> dict[str, Any]:
    raw = _exact_object(raw, {*_PHASE_CURVES, *_PHASE_COMPONENTS, "decode_paged_attention", "coverage"}, "phase_cost")
    has_decode = raw["decode_paged_attention"] is not None
    if has_decode != (raw["decode_collective"] is not None):
        raise ValueError("Decode attention and collective costs must be supplied together")
    result: dict[str, Any] = {}
    for component in _PHASE_CURVES:
        context = f"phase_cost.{component}"
        values = _exact_object(raw[component], {"new_token_points"}, context)
        normalized = _points(
            values["new_token_points"],
            context,
            {"new_tokens": positive_u64, "duration_us": positive_finite_number},
            ("new_tokens",),
        )
        durations = [point["duration_us"] for point in normalized]
        if len(normalized) < 2 or durations != sorted(durations):
            raise ValueError(f"{context} requires two or more anchors with nondecreasing duration")
        result[component] = {"new_token_points": normalized}
    for component in _PHASE_COMPONENTS:
        if component == "decode_collective" and not has_decode:
            result[component] = None
            continue
        values = _exact_object(raw[component], set(_PHASE_COEFFICIENTS), f"phase_cost.{component}")
        result[component] = {
            name: nonnegative_finite_number(values[name], f"phase_cost.{component}.{name}")
            for name in _PHASE_COEFFICIENTS
        }
    result["decode_paged_attention"] = _decode_paged_attention(raw["decode_paged_attention"]) if has_decode else None
    coverage = _exact_object(raw["coverage"], set(_PHASE_COVERAGE), "phase_cost.coverage")
    decode_range = {"min_decode_context_tokens", "max_decode_context_tokens"}
    if not has_decode and any(coverage[name] is not None for name in decode_range):
        raise ValueError("Absent Decode costs cannot declare measured context coverage")
    normalized_coverage = {
        name: None
        if not has_decode and name in decode_range
        else nonnegative_finite_number(coverage[name], f"phase_cost.coverage.{name}")
        for name in _PHASE_COVERAGE
    }
    if (
        normalized_coverage["min_new_tokens"] > normalized_coverage["max_new_tokens"]
        or normalized_coverage["min_context_tokens"] > normalized_coverage["max_context_tokens"]
        or normalized_coverage["min_attention_token_pairs"] > normalized_coverage["max_attention_token_pairs"]
        or (
            has_decode
            and normalized_coverage["min_decode_context_tokens"] > normalized_coverage["max_decode_context_tokens"]
        )
    ):
        raise ValueError("phase_cost.coverage minima must not exceed maxima")
    result["coverage"] = normalized_coverage
    return result


def _decode_paged_attention(raw: Any) -> dict[str, float | int]:
    decode_paged_fields = (
        "kernel_page_tokens",
        "fixed_us_per_iteration",
        "per_context_token_us",
        "per_effective_page_us",
    )
    decode_paged = _exact_object(raw, set(decode_paged_fields), "phase_cost.decode_paged_attention")
    kernel_page_tokens = positive_u64(
        decode_paged["kernel_page_tokens"],
        "phase decode paged-attention kernel_page_tokens",
    )
    normalized_decode_paged = {
        name: nonnegative_finite_number(decode_paged[name], f"phase_cost.decode_paged_attention.{name}")
        for name in decode_paged_fields
        if name != "kernel_page_tokens"
    }
    return {
        "kernel_page_tokens": kernel_page_tokens,
        **normalized_decode_paged,
    }
