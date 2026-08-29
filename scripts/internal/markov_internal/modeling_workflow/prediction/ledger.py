"""Compact Direct I/O/control ledger projected from one C++ result."""

from __future__ import annotations

from typing import Any

from ..io_model_contract import OPERATION_KINDS
from ..types import ModelRunResult


EFFECT_TO_KIND = {
    "prefetch_io_operation": "prefetch",
    "loadback": "load",
    "commit_device_to_host": "write_device_to_host",
    "commit_host_to_storage": "write_host_to_storage",
}

TOTAL_FIELDS = (
    "operation_count",
    "zero_payload_control_operation_count",
    "zero_payload_control_us",
    "page_count",
    "byte_count",
    "service_us",
    "control_us",
    "blocking_us",
    "storage_existing_page_count",
    "storage_new_page_count",
    "storage_existing_byte_count",
    "storage_new_byte_count",
)


def predicted_aggregates(result: ModelRunResult) -> tuple[dict[str, Any], list[str]]:
    """Read the C++ cost plan without reconstructing retired fitting features."""

    errors: list[str] = []
    by_kind = _empty_kind_aggregates()
    model_summary = result.artifacts.load_if_present(result.artifacts.model_summary_json)
    patch = _module_payload(model_summary, "HiCacheDagPatchModule", "hicache_dag_patch")
    attribution = patch.get("source_attribution")
    attribution_by_effect = {
        str(row.get("effect_id") or ""): row
        for row in (attribution.get("records") if isinstance(attribution, dict) else []) or []
        if isinstance(row, dict) and row.get("effect_id")
    }
    resources = patch.get("io_resources") if isinstance(patch.get("io_resources"), dict) else {}

    for raw in resources.get("costs") or []:
        if not isinstance(raw, dict):
            continue
        kind = EFFECT_TO_KIND.get(str(raw.get("effect_type") or ""))
        if kind is None:
            continue
        zero_payload = bool(raw.get("zero_payload_control"))
        status = str(raw.get("status") or "")
        if status == "not_required" and not zero_payload:
            continue
        if status != "ready":
            errors.append(f"predicted_cost_not_ready:{status}")
            continue

        byte_count = int(raw.get("effective_byte_count") or 0)
        if byte_count <= 0 and not zero_payload:
            continue
        page_count = int(raw.get("host_control_page_count") or raw.get("effective_page_count") or 0)
        if page_count <= 0 and byte_count > 0:
            page_bytes = int(resources.get("kv_bytes_per_page") or 0)
            page_count = byte_count // page_bytes if page_bytes > 0 else 0
        operation_count = max(0, int(raw.get("operation_count") or 0))
        existing_pages = int(raw.get("storage_existing_page_count") or 0)
        new_pages = int(raw.get("storage_new_page_count") or 0)
        existing_bytes = int(raw.get("storage_existing_byte_count") or 0)
        new_bytes = int(raw.get("storage_new_byte_count") or 0)
        if kind == "write_host_to_storage":
            if existing_pages + new_pages != page_count:
                errors.append("predicted_storage_residency_page_conservation_failed")
            if existing_bytes + new_bytes != byte_count:
                errors.append("predicted_storage_residency_byte_conservation_failed")

        service_us = int(raw.get("duration_us") or 0)
        control_us = int(raw.get("host_control_duration_us") or 0)
        aggregate = by_kind[kind]
        aggregate["operation_count"] += operation_count
        aggregate["zero_payload_control_operation_count"] += operation_count if zero_payload else 0
        aggregate["zero_payload_control_us"] += control_us if zero_payload else 0
        aggregate["page_count"] += page_count
        aggregate["byte_count"] += byte_count
        aggregate["service_us"] += service_us
        aggregate["control_us"] += control_us
        aggregate["blocking_us"] += service_us if kind == "prefetch" else 0
        aggregate["storage_existing_page_count"] += existing_pages
        aggregate["storage_new_page_count"] += new_pages
        aggregate["storage_existing_byte_count"] += existing_bytes
        aggregate["storage_new_byte_count"] += new_bytes
        aggregate["records"].append(
            {
                "effect_id": raw.get("effect_id"),
                "effect_type": raw.get("effect_type"),
                "direction": raw.get("direction"),
                "zero_payload_control": zero_payload,
                "target_effect_state": raw.get("target_effect_state"),
                "resource_scope": raw.get("resource_scope"),
                "resource_lane": raw.get("resource_lane"),
                "logical_order_epoch": int(raw.get("logical_order_epoch") or 0),
                "operation_count": operation_count,
                "page_count": page_count,
                "byte_count": byte_count,
                "storage_existing_page_count": existing_pages,
                "storage_new_page_count": new_pages,
                "storage_existing_byte_count": existing_bytes,
                "storage_new_byte_count": new_bytes,
                "service_us": service_us,
                "control_us": control_us,
                "blocking_us": service_us if kind == "prefetch" else 0,
                "calibration_setup_us": int(raw.get("calibration_setup_us") or 0),
                "calibration_transfer_us": int(raw.get("calibration_transfer_us") or 0),
                "runtime_scale": float(raw.get("runtime_scale") or 0.0),
                "storage_existing_service_us": int(raw.get("storage_existing_service_us") or 0),
                "storage_new_service_us": int(raw.get("storage_new_service_us") or 0),
                "host_control_fixed_us": int(raw.get("host_control_fixed_us") or 0),
                "host_control_page_us": int(raw.get("host_control_page_us") or 0),
                **_source_carrier_fields(attribution_by_effect.get(str(raw.get("effect_id") or ""))),
            }
        )

    return {
        "status": "READY" if not errors else "NOT_READY",
        "errors": sorted(set(errors)),
        "by_kind": by_kind,
        "totals": {field: sum(int(row[field]) for row in by_kind.values()) for field in TOTAL_FIELDS},
        "resource_status": resources.get("status"),
    }, errors


def _empty_kind_aggregates() -> dict[str, dict[str, Any]]:
    return {kind: {**{field: 0 for field in TOTAL_FIELDS}, "records": []} for kind in OPERATION_KINDS}


def _source_carrier_fields(attribution: dict[str, Any] | None) -> dict[str, Any]:
    if not attribution:
        return {
            "source_carrier_state": "unobservable",
            "source_timing_fact_node_ids": [],
            "source_io_operation_record_ids": [],
        }
    return {
        "source_carrier_state": attribution.get("source_carrier_state"),
        "source_timing_fact_node_ids": list(attribution.get("timing_fact_nodes") or []),
        "source_io_operation_record_ids": list(attribution.get("io_operation_record_ids") or []),
    }


def _module_payload(summary: dict[str, Any], name: str, key: str) -> dict[str, Any]:
    for module in summary.get("modules") or []:
        if isinstance(module, dict) and module.get("name") == name and isinstance(module.get(key), dict):
            return module[key]
    return {}
