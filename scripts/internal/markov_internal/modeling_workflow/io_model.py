"""Explicit HiCache geometry and unified I/O/control plus phase cost contract."""

from __future__ import annotations

import copy
from dataclasses import dataclass
import math
from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.paths import require_repo_path
from .io_model_contract import MAX_U64, positive_u64
from .io_model_validation import (
    required_control_models,
    required_resource_lanes,
    required_service_models,
)


@dataclass(frozen=True)
class HiCacheIoModel:
    """Validated numerical fields for the workflow-wide HiCache cost model."""

    source_path: Path
    fields: dict[str, Any]

    @classmethod
    def load(cls, path: Path) -> HiCacheIoModel:
        """Load the one canonical HiCache model contract without conversion."""

        resolved = require_repo_path(path)
        if not resolved.is_file():
            raise FileNotFoundError(f"missing HiCache model: {resolved}")
        raw = load_json(resolved)
        if not isinstance(raw, dict):
            raise TypeError(f"HiCache model must be a JSON object: {resolved}")
        return cls.from_raw(resolved, raw)

    @classmethod
    def from_raw(cls, path: Path, raw: dict[str, Any]) -> HiCacheIoModel:
        service_models = required_service_models(raw.get("service_models"))
        control_models = required_control_models(raw.get("control_models"))
        resource_lanes = required_resource_lanes(raw.get("resource_lanes"))
        phase_cost = _phase_cost(raw.get("phase_cost"))
        io_coverage = _io_coverage(raw.get("io_coverage"), service_models)
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
            "io_coverage": io_coverage,
        }
        return cls(source_path=path, fields=fields)

    def kv_bytes_per_page(self, page_size: int) -> int:
        token_bytes = self.fields["kv_bytes_per_token_per_rank"]
        if page_size <= 0:
            raise ValueError("HiCache model requires a positive target page_size")
        if page_size > MAX_U64 // token_bytes:
            raise OverflowError("target page_size * kv_bytes_per_token_per_rank exceeds uint64")
        return page_size * token_bytes

    def narrow_config(self, page_size: int, prefetch_policy: str) -> dict[str, Any]:
        page_bytes = self.kv_bytes_per_page(page_size)
        service_models = {}
        for kind, values in self.fields["service_models"].items():
            narrowed = copy.deepcopy(values)
            if kind == "write_host_to_storage":
                existing = narrowed.pop("existing_runtime_scale_points")
                narrowed["existing_runtime_scale"] = _runtime_scale(existing, page_bytes)
            else:
                points = narrowed.pop("runtime_scale_points")
                narrowed["runtime_scale"] = _runtime_scale(points, page_bytes)
            service_models[kind] = narrowed
        controls = {kind: dict(values) for kind, values in self.fields["control_models"].items()}
        check = controls["prefetch"].pop("state_check_us_per_operation", None)
        if prefetch_policy not in {"best_effort", "wait_complete", "timeout"}:
            raise ValueError("target prefetch policy must identify the actual control path")
        if prefetch_policy != "best_effort":
            if check is None:
                raise ValueError(
                    "target prefetch policy needs an unobserved state-check control primitive; "
                    "prepare this group first"
                )
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
        return result

    def domain_status(self, page_size: int, by_kind: dict[str, Any]) -> dict[str, Any]:
        """Report calibration-domain use; never change or reject a prediction."""

        page_bytes = self.kv_bytes_per_page(page_size)
        coverage = self.fields["io_coverage"]
        page_domain = coverage["page_bytes"]
        page_inside = page_domain["min"] <= page_bytes <= page_domain["max"]
        call_checks = []
        missing_families = set()
        outside_count = 0
        for family, aggregate in by_kind.items():
            domain = coverage["service_call_bytes"].get(family)
            for record in aggregate.get("records") or []:
                for call_bytes in _predicted_call_bytes(family, record, page_bytes):
                    if domain is None:
                        missing_families.add(family)
                        inside = None
                    else:
                        inside = domain["min"] <= call_bytes <= domain["max"]
                        outside_count += inside is False
                    call_checks.append({"family": family, "bytes": call_bytes, "inside": inside})
        if not page_inside or outside_count:
            status = "outside_observed_domain"
        elif missing_families:
            status = "unverified"
        else:
            status = "inside_observed_domain"
        return {
            "status": status,
            "page_bytes": {
                "value": page_bytes,
                "min": page_domain["min"],
                "max": page_domain["max"],
                "inside": page_inside,
            },
            "service_calls": {
                "checked_count": len(call_checks),
                "outside_count": outside_count,
                "unverified_families": sorted(missing_families),
                "observed_domain": coverage["service_call_bytes"],
            },
            "outside_domain_behavior": coverage["outside_domain_behavior"],
        }


def _runtime_scale(points: list[dict[str, Any]], page_bytes: int) -> float:
    if page_bytes <= points[0]["page_bytes"]:
        return float(points[0]["runtime_scale"])
    if page_bytes >= points[-1]["page_bytes"]:
        return float(points[-1]["runtime_scale"])
    for left, right in zip(points, points[1:]):
        if page_bytes > right["page_bytes"]:
            continue
        position = math.log(page_bytes / left["page_bytes"]) / math.log(right["page_bytes"] / left["page_bytes"])
        return math.exp(
            math.log(left["runtime_scale"])
            + position * math.log(right["runtime_scale"] / left["runtime_scale"])
        )
    raise RuntimeError("runtime scale interpolation failed")


def _io_coverage(raw: Any, service_models: dict[str, dict[str, Any]]) -> dict[str, Any]:
    if raw is None:
        anchors = sorted({
            int(point["page_bytes"])
            for service in service_models.values()
            for field in ("runtime_scale_points", "existing_runtime_scale_points")
            for point in service.get(field, [])
        })
        if not anchors:
            raise ValueError("HiCache model has no I/O page-byte calibration domain")
        return {
            "source": "legacy_model_endpoints",
            "page_bytes": {"min": anchors[0], "max": anchors[-1], "anchors": anchors},
            "service_call_bytes": {},
            "outside_domain_behavior": "page endpoint clamp; service-call domain was not recorded",
        }
    expected = {"source", "page_bytes", "service_call_bytes", "outside_domain_behavior"}
    if not isinstance(raw, dict) or set(raw) != expected:
        raise ValueError("io_coverage must contain the canonical domain fields")
    page = raw["page_bytes"]
    if not isinstance(page, dict) or set(page) != {"min", "max", "anchors"}:
        raise ValueError("io_coverage.page_bytes has an invalid contract")
    anchors = [positive_u64(value, "io_coverage.page_bytes.anchor") for value in page["anchors"]]
    if anchors != sorted(set(anchors)) or not anchors:
        raise ValueError("io_coverage page-byte anchors must be increasing and unique")
    if (
        positive_u64(page["min"], "io_coverage.page_bytes.min") != anchors[0]
        or positive_u64(page["max"], "io_coverage.page_bytes.max") != anchors[-1]
    ):
        raise ValueError("io_coverage page-byte bounds must match its anchors")
    calls = raw["service_call_bytes"]
    if not isinstance(calls, dict):
        raise ValueError("io_coverage.service_call_bytes must be an object")
    normalized_calls = {}
    for family, domain in calls.items():
        if family not in service_models or not isinstance(domain, dict) or set(domain) != {"min", "max", "sample_count"}:
            raise ValueError("io_coverage contains an invalid service-call domain")
        minimum = float(domain["min"])
        maximum = float(domain["max"])
        sample_count = positive_u64(domain["sample_count"], "io_coverage.service_call_bytes.sample_count")
        if not math.isfinite(minimum) or not math.isfinite(maximum) or minimum <= 0 or minimum > maximum:
            raise ValueError("io_coverage service-call byte bounds must be finite and positive")
        normalized_calls[family] = {"min": minimum, "max": maximum, "sample_count": sample_count}
    return {
        "source": str(raw["source"]),
        "page_bytes": {"min": anchors[0], "max": anchors[-1], "anchors": anchors},
        "service_call_bytes": normalized_calls,
        "outside_domain_behavior": str(raw["outside_domain_behavior"]),
    }


def _predicted_call_bytes(family: str, record: dict[str, Any], page_bytes: int) -> list[float]:
    if family in {"prefetch", "write_host_to_storage"}:
        return [
            float(batch.get("page_count") or 0) * page_bytes
            for batch in record.get("storage_service_batches") or []
            if float(batch.get("page_count") or 0) > 0
        ]
    operations = int(record.get("operation_count") or 0)
    byte_count = float(record.get("byte_count") or 0)
    return [byte_count / operations] * operations if operations > 0 and byte_count > 0 else []


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
    if raw is None:
        raise ValueError("HiCache model requires phase_cost")
    if not isinstance(raw, dict) or set(raw) != {
        *_PHASE_CURVES,
        *_PHASE_COMPONENTS,
        "decode_paged_attention",
        "coverage",
    }:
        raise ValueError("phase_cost must contain the canonical variable components and coverage")
    result: dict[str, Any] = {}
    for component in _PHASE_CURVES:
        values = raw[component]
        if not isinstance(values, dict) or set(values) != {"new_token_points"}:
            raise ValueError(f"phase_cost.{component} must contain one new-token curve")
        points = values["new_token_points"]
        if not isinstance(points, list) or len(points) < 2:
            raise ValueError(f"phase_cost.{component}.new_token_points requires two measured anchors")
        normalized = [
            {
                "new_tokens": positive_u64(point.get("new_tokens"), f"phase_cost.{component}.new_tokens"),
                "duration_us": float(point.get("duration_us")),
            }
            for point in points
            if isinstance(point, dict) and set(point) == {"new_tokens", "duration_us"}
        ]
        tokens = [point["new_tokens"] for point in normalized]
        durations = [point["duration_us"] for point in normalized]
        if (len(normalized) != len(points) or tokens != sorted(set(tokens))
                or any(not math.isfinite(value) or value <= 0.0 for value in durations)
                or durations != sorted(durations)):
            raise ValueError(f"phase_cost.{component}.new_token_points must be increasing and positive")
        result[component] = {"new_token_points": normalized}
    for component in _PHASE_COMPONENTS:
        values = raw[component]
        if not isinstance(values, dict) or set(values) != set(_PHASE_COEFFICIENTS):
            raise ValueError(f"phase_cost.{component} must contain the four canonical coefficients")
        normalized = {name: float(values[name]) for name in _PHASE_COEFFICIENTS}
        if any(not math.isfinite(value) or value < 0.0 for value in normalized.values()):
            raise ValueError(f"phase_cost.{component} coefficients must be finite and non-negative")
        result[component] = normalized
    result["decode_paged_attention"] = _decode_paged_attention(raw["decode_paged_attention"])
    coverage = raw["coverage"]
    if not isinstance(coverage, dict) or set(coverage) != set(_PHASE_COVERAGE):
        raise ValueError("phase_cost.coverage must contain the canonical feature bounds")
    normalized_coverage = {name: float(coverage[name]) for name in _PHASE_COVERAGE}
    if any(not math.isfinite(value) or value < 0.0 for value in normalized_coverage.values()):
        raise ValueError("phase_cost.coverage bounds must be finite and non-negative")
    if (
        normalized_coverage["min_new_tokens"] > normalized_coverage["max_new_tokens"]
        or normalized_coverage["min_context_tokens"] > normalized_coverage["max_context_tokens"]
        or normalized_coverage["min_attention_token_pairs"] > normalized_coverage["max_attention_token_pairs"]
        or normalized_coverage["min_decode_context_tokens"] > normalized_coverage["max_decode_context_tokens"]
    ):
        raise ValueError("phase_cost.coverage minima must not exceed maxima")
    result["coverage"] = normalized_coverage
    return result


def _decode_paged_attention(raw: Any) -> dict[str, float | int]:
    decode_paged = raw
    decode_paged_fields = {
        "kernel_page_tokens",
        "fixed_us_per_iteration",
        "per_context_token_us",
        "per_effective_page_us",
    }
    if not isinstance(decode_paged, dict) or set(decode_paged) != decode_paged_fields:
        raise ValueError("phase_cost.decode_paged_attention has an invalid contract")
    kernel_page_tokens = positive_u64(
        decode_paged["kernel_page_tokens"],
        "phase decode paged-attention kernel_page_tokens",
    )
    normalized_decode_paged = {
        name: float(decode_paged[name])
        for name in decode_paged_fields
        if name != "kernel_page_tokens"
    }
    if any(not math.isfinite(value) or value < 0.0 for value in normalized_decode_paged.values()):
        raise ValueError("phase decode paged-attention coefficients must be finite and non-negative")
    return {
        "kernel_page_tokens": kernel_page_tokens,
        **normalized_decode_paged,
    }
