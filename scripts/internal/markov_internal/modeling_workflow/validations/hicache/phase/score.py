"""Score source-only Prefill/Decode work against a target-only observation."""

from __future__ import annotations

from pathlib import Path
import math
from typing import Any

from .....common.io import load_json
from .....modeling.backend import append_option, execute_trace_graph
from .....modeling.cpp_config import trace_graph_executable
from .....modeling.workload import discover_workload_window
from ....types import ProfileRunRef


# Evaluation-only gate: five times the 4,490 us maximum combined-compute
# run-to-run deviation in the snapshot-free phase noise pilot.
PHASE_DELTA_MATERIAL_THRESHOLD_US = 22_450


def extract_target_phase_observation(
    target: ProfileRunRef,
    output_dir: Path,
    *,
    threads: int,
    file_threads: int,
) -> dict[str, Any]:
    """Run the compact target extractor after prediction; never feed it back to C++."""

    summary_path = output_dir / "run_summary.phase_carrier.json"
    if not summary_path.is_file():
        output_dir.mkdir(parents=True, exist_ok=True)
        command = [
            str(trace_graph_executable({"cpp_trace_graph": {"backend_kind": "validation"}})),
            "--profile-manifest",
            str(target.manifest_path),
            "--run-summary",
            str(summary_path),
            "--hicache-canonical-observed-phase-scope",
        ]
        append_option(command, "--threads", threads)
        append_option(command, "--file-threads", file_threads)
        window = discover_workload_window({}, target.manifest_path)
        if window is not None:
            append_option(command, "--trace-window-start-us", window.start_ns // 1000)
            append_option(command, "--trace-window-end-us", window.end_ns // 1000)
        execute_trace_graph(command)
    payload = load_json(summary_path)
    observed = payload.get("source_phase_observations") if isinstance(payload, dict) else None
    if not isinstance(observed, dict):
        raise ValueError(f"target phase extractor produced no observation: {target.label}")
    modules = payload.get("module_results") if isinstance(payload, dict) else None
    carrier = modules.get("hicache_observed_phase_carrier") if isinstance(modules, dict) else None
    if not isinstance(carrier, dict) or carrier.get("status") != "applied":
        raise ValueError(f"target phase carrier canonicalization failed: {target.label}")
    return payload


def compare_phase_work(
    prediction_summary: dict[str, Any], target_payload: dict[str, Any], *, include_oracle_costs: bool = False
) -> dict[str, Any]:
    """Compare canonical request work; target timing remains score-only."""

    modules = prediction_summary.get("module_results") if isinstance(prediction_summary, dict) else None
    hicache = modules.get("hicache") if isinstance(modules, dict) else None
    phase_work = hicache.get("phase_work") if isinstance(hicache, dict) else None
    if not isinstance(phase_work, dict):
        return _not_ready("prediction_phase_work_missing")
    target_observation = target_payload.get("source_phase_observations")
    if not isinstance(target_observation, dict):
        target_observation = target_payload
    observed_rows = target_observation.get("observations")
    if target_observation.get("status") != "ready" or not isinstance(observed_rows, list):
        return _not_ready("target_phase_observation_not_ready")

    predicted_prefills = {
        (int(row["logical_input"]), str(row["request_id"])): (
            int(row["prompt_token_count"]),
            int(row["reusable_prefix_token_count"]),
            int(row["prefill_token_count"]),
        )
        for row in phase_work.get("prefills") or []
    }
    predicted_decodes = {
        (int(row["logical_input"]), str(row["request_id"])): int(row["iteration_count"])
        for row in phase_work.get("decodes") or []
    }
    target_prefills: dict[tuple[int, str], tuple[int, int, int]] = {}
    target_decodes: dict[tuple[int, str], int] = {}
    invalid_target_batch_count = 0
    for row in observed_rows:
        request_ids = row.get("request_ids")
        if not isinstance(request_ids, list) or len(request_ids) != 1:
            invalid_target_batch_count += 1
            continue
        key = (int(row["logical_input"]), str(request_ids[0]))
        prompt = int(row["prompt_token_count"])
        prefill = int(row["prefill_token_count"])
        target_prefills[key] = (prompt, prompt - prefill, prefill)
        target_decodes[key] = int(row["decode_iteration_count"])

    prefill_keys_exact = predicted_prefills.keys() == target_prefills.keys()
    decode_keys_exact = predicted_decodes.keys() == target_decodes.keys()
    prefill_mismatch_count = sum(
        predicted_prefills.get(key) != value for key, value in target_prefills.items()
    ) + len(predicted_prefills.keys() - target_prefills.keys())
    decode_mismatch_count = sum(
        predicted_decodes.get(key) != value for key, value in target_decodes.items()
    ) + len(predicted_decodes.keys() - target_decodes.keys())
    work_exact = (
        phase_work.get("status") == "ready"
        and invalid_target_batch_count == 0
        and prefill_keys_exact
        and decode_keys_exact
        and prefill_mismatch_count == 0
        and decode_mismatch_count == 0
    )
    carrier_score = _compare_phase_carrier(prediction_summary, target_payload)
    exact = work_exact and carrier_score["exact"]
    cost_score = (
        _compare_phase_cost(phase_work, observed_rows, include_oracle_costs=include_oracle_costs)
        if prefill_keys_exact and decode_keys_exact
        else _cost_not_ready()
    )
    return {
        "ready": True,
        "structure_exact": exact,
        "predicted_prefill_count": len(predicted_prefills),
        "target_prefill_count": len(target_prefills),
        "predicted_decode_count": len(predicted_decodes),
        "target_decode_count": len(target_decodes),
        "prefill_mismatch_count": prefill_mismatch_count,
        "decode_mismatch_count": decode_mismatch_count,
        "invalid_target_batch_count": invalid_target_batch_count,
        "work_structure_exact": work_exact,
        "carrier_structure": carrier_score,
        "blockers": [] if exact else ["predicted_target_phase_structure_mismatch"],
        "cost": cost_score,
        "gap_excluded_scope": _scope_score(prediction_summary, target_payload),
    }


def _not_ready(blocker: str) -> dict[str, Any]:
    return {
        "ready": False,
        "structure_exact": False,
        "work_structure_exact": False,
        "carrier_structure": {"ready": False, "exact": False},
        "prefill_mismatch_count": 0,
        "decode_mismatch_count": 0,
        "blockers": [blocker],
        "cost": _cost_not_ready(),
        "gap_excluded_scope": {"ready": False},
    }


def _compare_phase_carrier(
    prediction_summary: dict[str, Any], target_payload: dict[str, Any]
) -> dict[str, Any]:
    predicted_modules = prediction_summary.get("module_results")
    predicted_patch = (
        predicted_modules.get("hicache_dag_patch") if isinstance(predicted_modules, dict) else None
    )
    predicted = predicted_patch.get("phase_carrier") if isinstance(predicted_patch, dict) else None
    target_modules = target_payload.get("module_results")
    target_module = (
        target_modules.get("hicache_observed_phase_carrier")
        if isinstance(target_modules, dict)
        else None
    )
    target = target_module.get("carrier") if isinstance(target_module, dict) else None
    if not isinstance(predicted, dict) or not isinstance(target, dict):
        return {"ready": False, "exact": False, "blockers": ["phase_carrier_audit_missing"]}

    count_fields = (
        "request_rank_count",
        "request_count",
        "synthetic_carrier_count",
        "dependency_count",
    )
    mismatches = {
        field: {"predicted": int(predicted.get(field) or 0), "target": int(target.get(field) or 0)}
        for field in count_fields
        if int(predicted.get(field) or 0) != int(target.get(field) or 0)
    }
    ready = all(
        (
            predicted.get("status") == "ready",
            target.get("status") == "ready",
            predicted_patch.get("phase_patch_status") == "ready",
            predicted_patch.get("topology_valid") is True,
            target_module.get("status") == "applied",
            target_module.get("topology_valid") is True,
            int(predicted.get("owner_conflict_count") or 0) == 0,
            int(target.get("owner_conflict_count") or 0) == 0,
            not predicted.get("blockers"),
            not target.get("blockers"),
        )
    )
    exact = ready and not mismatches
    return {
        "ready": ready,
        "exact": exact,
        "count_mismatches": mismatches,
        "predicted_request_rank_count": int(predicted.get("request_rank_count") or 0),
        "target_request_rank_count": int(target.get("request_rank_count") or 0),
        "predicted_synthetic_carrier_count": int(predicted.get("synthetic_carrier_count") or 0),
        "target_synthetic_carrier_count": int(target.get("synthetic_carrier_count") or 0),
        "blockers": [] if exact else ["phase_carrier_or_topology_mismatch"],
    }


def _scope_score(prediction_summary: dict[str, Any], target_payload: dict[str, Any]) -> dict[str, Any]:
    predicted = int(prediction_summary.get("simulated_gap_excluded_e2e_us") or 0)
    target = int(target_payload.get("simulated_gap_excluded_e2e_us") or 0)
    ready = predicted > 0 and target > 0
    return {
        "ready": ready,
        "predicted_us": predicted,
        "target_us": target,
        "absolute_error_us": abs(predicted - target) if ready else None,
        "ape": abs(predicted - target) / target if ready else None,
        "target_opened_after_prediction": True,
    }


def _compare_phase_cost(
    phase_work: dict[str, Any], observed_rows: list[dict[str, Any]], *, include_oracle_costs: bool
) -> dict[str, Any]:
    collective: dict[tuple[str, str], int] = {}
    rank_collective: dict[tuple[str, int, str], int] = {}
    targets: dict[tuple[int, str], dict[str, int]] = {}
    for row in observed_rows:
        request_ids = row.get("request_ids")
        if not isinstance(request_ids, list) or len(request_ids) != 1:
            continue
        request = str(request_ids[0])
        key = (int(row["logical_input"]), request)
        targets[key] = {
            "prefill_kernel": int(row["prefill_kernel_duration_us"]),
            "prefill_common_kernel": int(row["prefill_common_kernel_duration_us"]),
            "prefill_prefix_attention": int(row["prefill_prefix_attention_duration_us"]),
            "prefill_submit": int(row["prefill_submit_cpu_duration_us"]),
            "decode_kernel": int(row["decode_kernel_duration_us"]),
            "decode_paged_attention": _decode_paged_attention_duration(row),
            "decode_submit": int(row["decode_submit_cpu_duration_us"]),
        }
        for phase in ("prefill", "decode"):
            value = int(row[f"{phase}_collective_duration_us"])
            pair = (phase, request)
            collective[pair] = min(value, collective.get(pair, value))
            rank_collective[(phase, key[0], request)] = value

    samples: dict[str, list[tuple[int, int]]] = {
        "prefill_kernel": [],
        "prefill_collective": [],
        "prefill_compute": [],
        "prefill_submit_template": [],
        "decode_paged_attention": [],
        "decode_kernel": [],
        "decode_collective": [],
        "decode_compute": [],
        "decode_submit_template": [],
        "combined_compute": [],
    }
    prefills = {
        (int(row["logical_input"]), str(row["request_id"])): row for row in phase_work.get("prefills") or []
    }
    decodes = {
        (int(row["logical_input"]), str(row["request_id"])): row for row in phase_work.get("decodes") or []
    }
    source_prefill_collective: dict[str, int] = {}
    source_decode_collective: dict[str, int] = {}
    for rows, destination in (
        (prefills.values(), source_prefill_collective),
        (decodes.values(), source_decode_collective),
    ):
        for row in rows:
            request = str(row["request_id"])
            value = int(row["collective_cost"]["source_duration_us"])
            destination[request] = min(value, destination.get(request, value))
    device_oracle_costs: list[dict[str, Any]] = []
    all_owner_device_oracle_costs: list[dict[str, Any]] = []
    control_oracle_costs: list[dict[str, Any]] = []
    combined_predicted_us = 0
    combined_target_us = 0
    combined_source_us = 0
    for key, target in targets.items():
        prefill = prefills[key]
        decode = decodes[key]
        request = key[1]
        predicted_prefill_kernel = int(prefill["kernel_cost"]["predicted_duration_us"])
        target_prefill_kernel = target["prefill_kernel"]
        predicted_prefill_collective = int(prefill["collective_cost"]["predicted_duration_us"])
        target_prefill_collective = collective[("prefill", request)]
        predicted_prefill_submit = int(prefill["submit_cost"]["predicted_duration_us"])
        predicted_decode_kernel = int(decode["kernel_cost"]["predicted_duration_us"])
        predicted_decode_paged_attention = int(decode["predicted_paged_attention_duration_us"])
        target_decode_kernel = target["decode_kernel"]
        predicted_decode_collective = int(decode["collective_cost"]["predicted_duration_us"])
        target_decode_collective = collective[("decode", request)]
        predicted_decode_submit = int(decode["submit_cost"]["predicted_duration_us"])
        source_prefill_compute = int(prefill["kernel_cost"]["source_duration_us"]) + source_prefill_collective[request]
        source_decode_compute = int(decode["kernel_cost"]["source_duration_us"]) + source_decode_collective[request]
        rank, request_id = key
        prefill_effect = f"hicache_phase:{request_id}:prefill:{rank}:"
        decode_effect = f"hicache_phase:{request_id}:decode:{rank}:"
        if include_oracle_costs:
            device_oracle_costs.extend(
                (
                    {"effect_id": prefill_effect + "common_kernel", "duration_us": target["prefill_common_kernel"]},
                    {"effect_id": prefill_effect + "prefix_attention", "duration_us": target["prefill_prefix_attention"]},
                    {"effect_id": prefill_effect + "collective", "duration_us": target_prefill_collective},
                    {"effect_id": decode_effect + "kernel", "duration_us": target_decode_kernel},
                    {"effect_id": decode_effect + "collective", "duration_us": target_decode_collective},
                )
            )
            all_owner_device_oracle_costs.extend(
                (
                    {"effect_id": prefill_effect + "common_kernel", "duration_us": target["prefill_common_kernel"]},
                    {"effect_id": prefill_effect + "prefix_attention", "duration_us": target["prefill_prefix_attention"]},
                    {
                        "effect_id": prefill_effect + "collective",
                        "duration_us": rank_collective[("prefill", rank, request)],
                    },
                    {"effect_id": decode_effect + "kernel", "duration_us": target_decode_kernel},
                    {
                        "effect_id": decode_effect + "collective",
                        "duration_us": rank_collective[("decode", rank, request)],
                    },
                )
            )
            control_oracle_costs.extend(
                (
                    {"effect_id": prefill_effect + "submit", "duration_us": target["prefill_submit"]},
                    {"effect_id": decode_effect + "submit", "duration_us": target["decode_submit"]},
                )
            )
        pairs = {
            "prefill_kernel": (predicted_prefill_kernel, target_prefill_kernel),
            "prefill_collective": (predicted_prefill_collective, target_prefill_collective),
            "prefill_compute": (
                predicted_prefill_kernel + predicted_prefill_collective,
                target_prefill_kernel + target_prefill_collective,
            ),
            "prefill_submit_template": (predicted_prefill_submit, target["prefill_submit"]),
            "decode_paged_attention": (
                predicted_decode_paged_attention,
                target["decode_paged_attention"],
            ),
            "decode_kernel": (predicted_decode_kernel, target_decode_kernel),
            "decode_collective": (predicted_decode_collective, target_decode_collective),
            "decode_compute": (
                predicted_decode_kernel + predicted_decode_collective,
                target_decode_kernel + target_decode_collective,
            ),
            "decode_submit_template": (predicted_decode_submit, target["decode_submit"]),
            "combined_compute": (
                predicted_prefill_kernel + predicted_prefill_collective + predicted_decode_kernel + predicted_decode_collective,
                target_prefill_kernel + target_prefill_collective + target_decode_kernel + target_decode_collective,
            ),
        }
        for name, pair in pairs.items():
            samples[name].append(pair)
        combined_predicted_us += pairs["combined_compute"][0]
        combined_target_us += pairs["combined_compute"][1]
        combined_source_us += source_prefill_compute + source_decode_compute
    predicted_delta = combined_predicted_us - combined_source_us
    target_delta = combined_target_us - combined_source_us
    result = {
        "ready": phase_work.get("cost_status") == "ready" and bool(targets),
        "sample_count": len(targets),
        "metrics": {name: _error_metrics(values) for name, values in samples.items()},
        "combined_delta": {
            "predicted_us": predicted_delta,
            "target_us": target_delta,
            "absolute_error_us": abs(predicted_delta - target_delta),
            "material_threshold_us": PHASE_DELTA_MATERIAL_THRESHOLD_US,
            "large_change": abs(target_delta) > PHASE_DELTA_MATERIAL_THRESHOLD_US,
            "direction_correct": _same_direction(predicted_delta, target_delta),
        },
        "target_cost_used_for_parameters": False,
        "target_cost_opened_after_prediction": True,
    }
    if include_oracle_costs:
        result.update(
            device_oracle_costs=device_oracle_costs,
            all_owner_device_oracle_costs=all_owner_device_oracle_costs,
            control_oracle_costs=control_oracle_costs,
        )
    return result


def _decode_paged_attention_duration(row: dict[str, Any]) -> int:
    families = row.get("decode_kernel_families")
    if not isinstance(families, dict):
        return 0
    return sum(
        int(value.get("duration_us") or 0)
        for name, value in families.items()
        if isinstance(value, dict)
        and "attention" in name.lower()
    )


def _same_direction(predicted: int, target: int) -> bool:
    if target == 0:
        return predicted == 0
    return (predicted > 0) == (target > 0)


def _error_metrics(samples: list[tuple[int, int]]) -> dict[str, float | int | None]:
    if not samples:
        return {"sample_count": 0, "wape": None, "p90_ape": None, "weighted_l1_us": None}
    absolute = [abs(predicted - target) for predicted, target in samples]
    ape = sorted(error / max(abs(target), 1) for error, (_, target) in zip(absolute, samples))
    return {
        "sample_count": len(samples),
        "wape": sum(absolute) / max(sum(abs(target) for _, target in samples), 1),
        "p90_ape": ape[min(len(ape) - 1, math.ceil(0.9 * len(ape)) - 1)],
        "weighted_l1_us": sum(absolute),
        "target_total_us": sum(abs(target) for _, target in samples),
    }


def _cost_not_ready() -> dict[str, Any]:
    return {"ready": False, "sample_count": 0, "metrics": {}}
