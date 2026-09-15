"""Readiness of base plus the one fixed calibration; target work is not an input."""

from __future__ import annotations

from typing import Any

from .io_model_contract import OPERATION_KINDS, io_observation_ready
from .fixed_calibration import calibration_page_sizes
from .stability import repeat_duration_report, repeat_work_report


def _positive_service(capture: dict[str, Any], kind: str) -> list[dict[str, Any]]:
    return [
        row for row in capture["source_io_observations"]["observations"]
        if row.get("kind") == kind and io_observation_ready(row) and row["service_observed"]
        and row["service_page_count"] > 0 and row["service_us"] > 0
        and (kind != "write_host_to_storage" or row.get("storage_residency_observed"))
    ]


def scan_model_inputs(group, observations: list[dict[str, Any]]) -> dict[str, Any]:
    """Check only inputs required by the fixed model, never target coverage."""

    bases = [capture for capture in observations if capture["role"] == "base"]
    calibration = [capture for capture in observations if capture["role"] == "calibration"]
    missing: list[dict[str, str]] = []
    if group.physical is None:
        missing.append({"component": "physical", "reason": "platform_calibration_missing"})
    if len(bases) != len(group.sources):
        missing.append({"component": "base", "reason": "base_observations_incomplete"})
    endpoints = calibration_page_sizes(group)[0] if group.physical is not None else []
    calibration_by_page = {
        page: [capture for capture in calibration
               if any(int(row.get("page_size") or 0) == page
                      for row in capture["source_io_observations"]["observations"])]
        for page in endpoints
    }
    for page, captures in calibration_by_page.items():
        if len(captures) < group.budget.repeats:
            missing.append({"component": f"calibration/page_{page}", "reason": "endpoint_repeats_incomplete"})
        for capture in captures:
            source = str(capture["source_manifest"])
            for kind in OPERATION_KINDS:
                if not _positive_service(capture, kind):
                    missing.append({"component": f"service/{kind}/page_{page}",
                                    "reason": f"capture_work_missing:{source}"})
            h2s = _positive_service(capture, "write_host_to_storage")
            pure_existing = [row for row in h2s if int(row.get("storage_existing_page_count") or 0) > 0
                             and int(row.get("storage_new_page_count") or 0) == 0]
            if not pure_existing:
                missing.append({"component": f"service/write_host_to_storage/page_{page}",
                                "reason": f"pure_existing_anchor_missing:{source}"})
            new_sizes = {
                int(row["service_page_count"]) / len(row.get("storage_service_batches") or [])
                for row in h2s
                if int(row.get("storage_new_page_count") or 0) == int(row.get("service_page_count") or 0)
                and row.get("storage_service_batches")
            }
            if len(new_sizes) < 2:
                missing.append({"component": f"service/write_host_to_storage/page_{page}",
                                "reason": f"two_new_bytes_per_call_anchors_missing:{source}"})

    service_counts = {kind: sum(len(_positive_service(capture, kind)) for capture in calibration) for kind in OPERATION_KINDS}
    for kind, count in service_counts.items():
        if count == 0:
            missing.append({"component": f"service/{kind}", "reason": "positive_fixed_calibration_work_missing"})
        for page, captures in calibration_by_page.items():
            if not any(_positive_service(capture, kind) for capture in captures):
                missing.append({"component": f"service/{kind}/page_{page}", "reason": "endpoint_work_missing"})

    prefetch_rows = [row for capture in calibration for row in capture["source_io_observations"]["observations"]
                     if row.get("kind") == "prefetch" and row.get("terminal_control_observed")]
    if not prefetch_rows:
        missing.append({"component": "control/prefetch", "reason": "terminal_control_missing"})
    elif not any(row.get("progress_check_cpu_samples_us") for row in prefetch_rows):
        missing.append({"component": "control/prefetch", "reason": "state_check_missing"})
    load_rows = [row for capture in calibration for row in capture["source_io_observations"]["observations"]
                 if row.get("kind") == "load" and row.get("completed_tokens", 0) > 0
                 and row.get("admission_control_observed") and row.get("admission_control_us", 0) > 0]
    if not load_rows:
        missing.append({"component": "control/load", "reason": "admission_control_missing"})

    phase_sources = [capture for capture in [*bases, *calibration]
                     if capture.get("source_phase_observations", {}).get("status") == "ready"]
    if len(phase_sources) != len(bases) + len(calibration):
        missing.append({"component": "phase", "reason": "phase_observations_incomplete"})
    else:
        phase_rows = [row for capture in phase_sources for row in capture["source_phase_observations"]["observations"]]
        if len({int(row["prefill_token_count"]) for row in phase_rows}) < 2:
            missing.append({"component": "phase/prefill", "reason": "two_token_anchors_required"})
        if not any(int(row.get("prefill_prefix_attention_duration_us") or 0) > 0 for row in phase_rows):
            missing.append({"component": "phase/prefill", "reason": "prefix_attention_missing"})
        if not any(int(row.get("decode_iteration_count") or 0) > 0 for row in phase_rows):
            missing.append({"component": "phase/decode", "reason": "decode_work_missing"})

    repeat = repeat_work_report(calibration)
    duration_repeat = repeat_duration_report(calibration)
    if repeat["status"] == "work_changed":
        missing.append({"component": "calibration", "reason": "repeat_semantic_work_changed"})
    return {
        "status": "ready" if not missing else "data_limitation",
        "missing": missing,
        "base_profile_count": len(bases),
        "fixed_calibration_profile_count": len(calibration),
        "fixed_calibration_profiles_by_endpoint": {
            str(page): len(captures) for page, captures in calibration_by_page.items()
        },
        "service_observation_count": service_counts,
        "repeat_work": repeat,
        "repeat_duration": duration_repeat,
        "target_inputs": [],
        "accuracy_verified": False,
    }
