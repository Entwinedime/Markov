"""Minimum target-only evidence needed for historical static oracle-cost replay.

Ordinary HTTP scoring neither imports target costs nor constructs a target DAG.
Structure admission stays strict here; detailed component accuracy reports are retired.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from .....common.io import load_json
from .....modeling.backend import append_option, execute_trace_graph, trace_graph_executable
from ....types import ProfileRunRef
from ...final_dag.shape_compare import shapes_match


def observed_direct_cost(row: dict[str, Any]) -> tuple[int, int]:
    """One operation's canonical service/control clock, also used by oracle replay."""

    service = control = 0
    if row["service_observed"]:
        service = row["service_us"]
    if row["direction"] == "host_to_device" and row["admission_control_observed"] and row["completed_tokens"] > 0:
        control = row["admission_control_us"]
    if row["direction"] == "storage_to_host" and row["terminal_control_observed"]:
        control = row["terminal_explicit_cpu_us"]
    return service, control


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
            str(trace_graph_executable("validation")),
            "--profile-manifest",
            str(target.manifest_path),
            "--run-summary",
            str(summary_path),
            "--hicache-canonical-observed-phase-scope",
        ]
        append_option(command, "--threads", threads)
        append_option(command, "--file-threads", file_threads)
        window = target.workload_window
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


def compare_phase_work(prediction_summary: dict[str, Any], target_payload: dict[str, Any]) -> dict[str, Any]:
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
    work_exact = (
        phase_work.get("status") == "ready"
        and invalid_target_batch_count == 0
        and predicted_prefills == target_prefills
        and predicted_decodes == target_decodes
    )
    carrier_score = _compare_phase_carrier(prediction_summary, target_payload)
    exact = work_exact and carrier_score["exact"]
    return {
        "structure_exact": exact,
        "cost": phase_oracle_costs(observed_rows) if prefill_keys_exact and decode_keys_exact else {},
        "gap_excluded_scope": _scope_score(prediction_summary, target_payload),
    }


def _not_ready(blocker: str) -> dict[str, Any]:
    return {"structure_exact": False, "blockers": [blocker], "cost": {}, "gap_excluded_scope": {"ready": False}}


def _compare_phase_carrier(prediction_summary: dict[str, Any], target_payload: dict[str, Any]) -> dict[str, Any]:
    predicted_modules = prediction_summary.get("module_results")
    predicted_patch = predicted_modules.get("hicache_dag_patch") if isinstance(predicted_modules, dict) else None
    predicted = predicted_patch.get("phase_carrier") if isinstance(predicted_patch, dict) else None
    target_modules = target_payload.get("module_results")
    target_module = target_modules.get("hicache_observed_phase_carrier") if isinstance(target_modules, dict) else None
    target = target_module.get("carrier") if isinstance(target_module, dict) else None
    if not isinstance(predicted, dict) or not isinstance(target, dict):
        return {"ready": False, "exact": False, "blockers": ["phase_carrier_audit_missing"]}

    count_fields = (
        "request_rank_count",
        "request_count",
        "synthetic_carrier_count",
        "dependency_count",
    )
    counts_match = all(int(predicted.get(field) or 0) == int(target.get(field) or 0) for field in count_fields)
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
    exact = ready and counts_match
    return {"ready": ready, "exact": exact}


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


def _decode_paged_attention_duration(row: dict[str, Any]) -> int:
    families = row.get("decode_kernel_families")
    if not isinstance(families, dict):
        return 0
    return sum(
        int(value.get("duration_us") or 0)
        for name, value in families.items()
        if isinstance(value, dict) and "attention" in name.lower()
    )


def phase_oracle_costs(observed_rows: list[dict]) -> dict:
    """Keep measured kernel, rank/critical communication and submission costs separate."""
    rows = [row for row in observed_rows if len(row["request_ids"]) == 1]
    collective = {}
    for row in rows:
        request = row["request_ids"][0]
        for phase in ("prefill", "decode"):
            key = (phase, request)
            value = int(row[f"{phase}_collective_duration_us"])
            collective[key] = min(value, collective.get(key, value))

    device, owner_device, control = [], [], []
    for row in rows:
        rank, request = int(row["logical_input"]), row["request_ids"][0]
        for phase in ("prefill", "decode"):
            effect = f"hicache_phase:{request}:{phase}:{rank}:"
            families = ("common_kernel", "prefix_attention") if phase == "prefill" else ("kernel",)
            kernels = [
                {"effect_id": effect + family, "duration_us": int(row[f"{phase}_{family}_duration_us"])}
                for family in families
            ]
            if phase == "decode":
                kernels[0]["paged_attention_duration_us"] = _decode_paged_attention_duration(row)

            device.extend(kernels)
            owner_device.extend(dict(kernel) for kernel in kernels)
            device.append({"effect_id": effect + "collective", "duration_us": collective[(phase, request)]})
            owner_device.append(
                {"effect_id": effect + "collective", "duration_us": int(row[f"{phase}_collective_duration_us"])}
            )
            control.append({"effect_id": effect + "submit", "duration_us": int(row[f"{phase}_submit_cpu_duration_us"])})

    return {
        "device_oracle_costs": device,
        "all_owner_device_oracle_costs": owner_device,
        "control_oracle_costs": control,
    }


def prepare_replay_evidence(prediction: dict, run: dict, observed: dict, oracle: dict) -> dict:
    """Admit matching structures and retain only the costs consumed by replay."""
    phase = compare_phase_work(run, observed)
    totals = prediction["target_predicted"]["totals"]

    def total_cost(payload: dict) -> int:
        return sum(sum(observed_direct_cost(row)) for row in payload["source_io_observations"]["observations"])

    return {
        "structure_exact": shapes_match(prediction["shape"], oracle),
        "phase_structure_exact": phase["structure_exact"],
        "phase_score": phase,
        "direct": {
            "predicted_us": totals["service_us"] + totals["control_us"],
            "target_us": total_cost(observed),
            "base_us": total_cost(run),
        },
    }
