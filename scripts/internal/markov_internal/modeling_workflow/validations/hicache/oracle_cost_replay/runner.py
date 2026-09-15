"""Diagnostic replay of target-observed costs on predicted HiCache DAGs."""

from __future__ import annotations

import subprocess
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any

from .....common.io import load_json, write_json
from .....common.paths import ROOT_DIR, resolve_repo_path
from .....modeling.cpp_config import trace_graph_executable
from ....prediction.ledger import _module_payload
from .matching import _oracle_cost_record, build_pair_oracle_override, build_score_only_target_oracle_catalog, target_operation_cell


def run_suite(
    prediction_roots: dict[str, Path],
    score_rows: list[dict[str, Any]],
    source_runs: dict[str, dict],
    target_runs: dict[str, dict],
    output_dir: Path,
    *,
    source_config_id: str,
    jobs: int = 4,
    max_runs: int | None = None,
) -> dict[str, Any]:
    """Replay completed, scored predictions; never overwrite their artifacts."""

    candidates = [row for row in score_rows if row["source_config_id"] == source_config_id and not row["is_self"]]
    available_count = len(candidates)
    selected, eligible_count = _select_replay_scores(candidates, max_runs)
    ledgers = _ready_ledgers(prediction_roots, selected)
    work = []
    for ledger in sorted(ledgers, key=lambda row: str(row["model_run_id"])):
        model_id = ledger["model_run_id"]
        config = load_json(prediction_roots[model_id] / "model_runs" / model_id / "cpp_model_config.json")["hicache"]
        # KV geometry is declared metadata, not a fitted cost or predicted payload.
        token_bytes, remainder = divmod(config["kv_bytes_per_page"], config["page_size"])
        if remainder or token_bytes <= 0:
            raise ValueError("oracle projection requires integral positive KV bytes per token")
        target = target_operation_cell(ledger, target_runs[ledger["target_run_id"]], token_bytes)
        catalog = build_score_only_target_oracle_catalog(ledger, target, source_runs[model_id])
        override = build_pair_oracle_override(ledger, catalog)
        oracle_total = sum(row["service_us"] + row["control_us"] for row in override["costs"])
        if oracle_total != ledger["direct"]["target_us"]:
            raise ValueError(f"oracle costs differ from this evaluation's Direct clock: {model_id}")
        work.append((ledger, override, _direct_score(ledger)))
    rows: list[dict[str, Any]] = []
    with ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        futures = {
            pool.submit(_run_one, prediction_roots[ledger["model_run_id"]] / "model_runs" / ledger["model_run_id"],
                        ledger, override, score): ledger for ledger, override, score in work
        }
        for completed, future in enumerate(as_completed(futures), 1):
            row = future.result()
            rows.append(row)
            print(f"oracle-cost {completed}/{len(work)} {row['status']} {row['pair_id']}", flush=True)
    summary = _summary(sorted(rows, key=lambda row: str(row["model_run_id"])), len(work))
    summary.update(available_cell_count=available_count, eligible_cell_count=eligible_count,
                   excluded_nonexact_structure_count=available_count - eligible_count,
                   selected_cell_count=len(ledgers),
                   selection_complete=len(ledgers) == available_count,
                   selection_policy="strict_structure_exact_before_limit",
                   cpp_replay_count=sum(row.get("cpp_replay_count", 0) for row in rows),
                   cost_source="current_evaluation_observations", direct_clock_matches_scoring=True,
                   lane_binding="source_operation_to_logical_input_to_target_lane")
    if not summary["selection_complete"] and summary["status"] == "OK":
        summary["status"] = "PARTIAL"
    write_json(output_dir / f"artifacts/oracle_cost_replay/{source_config_id}/summary.json", summary)
    return summary


def _select_replay_scores(scores: list[dict[str, Any]], max_runs: int | None) -> tuple[list[dict[str, Any]], int]:
    """Apply the replay limit only after excluding non-exact structures."""

    eligible = [
        row
        for row in scores
        if row["status"] == "READY" and row["structure_exact"] and row["phase_structure_exact"]
    ]
    eligible.sort(key=lambda row: row["model_run_id"])
    selected = eligible[:max_runs] if max_runs is not None else eligible
    return selected, len(eligible)


def _ready_ledgers(prediction_roots: dict[str, Path], scores: list[dict[str, Any]]) -> list[dict[str, Any]]:
    if not scores or any(row["status"] != "READY" or not row["structure_exact"] or not row["phase_structure_exact"] for row in scores):
        raise ValueError("oracle-cost replay requires ready single-base cross ledgers")
    ready = []
    for score in scores:
        run_id = score["model_run_id"]
        path = prediction_roots[run_id] / "artifacts/debug_rows" / f"{run_id}.json"
        if not path.is_file():
            raise ValueError(f"oracle replay needs prediction --diagnostics full; no automatic rerun: {run_id}")
        details = load_json(path)
        if details["model_run_id"] != run_id or details["status"] != "READY" or details["target_predicted"]["totals"] != score["target_predicted"]["totals"]:
            raise ValueError(f"oracle detail differs from completed prediction: {run_id}")
        model_path = prediction_roots[run_id] / "model_runs" / run_id / "model_summary.json"
        if not model_path.is_file():
            raise ValueError(f"oracle replay needs full source attribution; no automatic rerun: {run_id}")
        patch = _module_payload(load_json(model_path), "HiCacheDagPatchModule", "hicache_dag_patch")
        attribution = {row["effect_id"]: row["io_operation_record_ids"] for row in patch["source_attribution"]["records"]}
        # Include source effects omitted by the target. A new zero-payload
        # Prefetch may have no own I/O carrier, but its scope still has source anchors.
        scope_records = [{"resource_scope": row["resource_scope"],
                          "source_io_operation_record_ids": attribution.get(row["effect_id"], [])}
                         for row in patch["io_resources"]["costs"]]
        ready.append({**score, "target_config_id": score["target_config"], "target_predicted": details["target_predicted"],
                      "source_scope_records": scope_records})
    return ready


def _direct_score(ledger: dict[str, Any]) -> dict[str, Any]:
    direct = ledger["direct"]
    predicted_us, actual_us, source_us = (direct[key] for key in ("predicted_us", "target_us", "base_us"))
    predicted_delta, actual_delta = predicted_us - source_us, actual_us - source_us
    return {
        "predicted_us": predicted_us,
        "actual_us": actual_us,
        "source_us": source_us,
        "predicted_delta_us": predicted_delta,
        "actual_delta_us": actual_delta,
        "large_change": abs(actual_delta) > 50_000,
        "direction_correct": (predicted_delta > 0) == (actual_delta > 0) if actual_delta else predicted_delta == 0,
    }


def _run_one(
    run_dir: Path,
    ledger: dict[str, Any],
    override: dict[str, Any],
    score: dict[str, Any],
) -> dict[str, Any]:
    identity = {
        field: ledger.get(field)
        for field in ("model_run_id", "pair_id", "workload_id", "source_config_id", "target_config_id")
    }
    runner = load_json(run_dir / "runner_config.json")
    original = load_json(run_dir / "run_summary.json")
    phase_score = ((ledger.get("phase_score") or {}).get("cost") or {})
    device_costs = phase_score.get("device_oracle_costs") or []
    owner_device_costs = phase_score.get("all_owner_device_oracle_costs") or []
    control_costs = phase_score.get("control_oracle_costs") or []
    if not device_costs or not owner_device_costs or not control_costs:
        return {
            **identity,
            "status": "ERROR",
            "errors": ["phase_oracle_cost_sets_missing"],
        }
    with TemporaryDirectory(prefix="markov_oracle_cost_") as raw_temp:
        temp = Path(raw_temp)
        direct_path = temp / "direct_costs.json"
        identity_path = temp / "identical_costs.json"
        compute_path = temp / "phase_device_costs.json"
        owner_path = temp / "phase_all_owner_costs.json"
        write_json(direct_path, override)
        predicted_records = [row for kind in ledger["target_predicted"]["by_kind"].values() for row in kind["records"]]
        write_json(identity_path, {"costs": [_oracle_cost_record(row, [row]) for row in predicted_records]})
        write_json(compute_path, {"phase_costs": device_costs})
        write_json(
            owner_path,
            {"phase_costs": owner_device_costs, "phase_control_costs": control_costs},
        )
        variants = {
            "identity": (identity_path, None),
            "direct": (direct_path, None),
            "phase_compute": (None, compute_path),
            "phase_owner": (None, owner_path),
            "direct_phase_compute": (direct_path, compute_path),
            "direct_phase_owner": (direct_path, owner_path),
        }
        replays: dict[str, dict[str, Any]] = {}
        replay_count = 0
        for name, (direct_override, phase_override) in variants.items():
            summary_path = temp / f"{name}_run_summary.json"
            completed = subprocess.run(
                _command(
                    runner,
                    run_dir / "cpp_model_config.json",
                    summary_path,
                    direct_override=direct_override,
                    phase_override=phase_override,
                ),
                cwd=ROOT_DIR,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )
            replay_count += 1
            if completed.returncode:
                return {
                    **identity,
                    "status": "ERROR",
                    "cpp_replay_count": replay_count,
                    "errors": [f"cpp_{name}_oracle_cost_replay_failed"],
                    "error_output_tail": completed.stdout[-4096:],
                }
            replays[name] = load_json(summary_path)
            if name == "identity" and (mismatches := _identity_mismatches(original, replays[name])):
                return {**identity, "status": "ERROR", "cpp_replay_count": replay_count,
                        "errors": ["identical_cost_replay_changed_prediction"], "identity_mismatches": mismatches}
    patches = {
        name: replay.get("module_results", {}).get("hicache_dag_patch", {})
        for name, replay in replays.items()
    }
    patch = patches["direct"]
    causal = patch.get("causal_timing_audit", {})
    expected_costs = len(override.get("costs") or [])
    causal_ready = (
        causal.get("status") == "ready" and causal.get("restored_exact") is True
        if expected_costs
        else causal.get("status") == "no_materialized_target_cost_nodes"
    )
    variant_ready: dict[str, bool] = {}
    for name, candidate in patches.items():
        direct_required = name in {"identity", "direct", "direct_phase_compute", "direct_phase_owner"}
        phase_required = name in {"phase_compute", "phase_owner", "direct_phase_compute", "direct_phase_owner"}
        direct_audit = candidate.get("io_resources", {}).get("oracle_cost_replay", {})
        phase_audit = candidate.get("phase_oracle_cost_replay", {})
        common_ready = all(
            (
                candidate.get("validation", {}).get("status") == "ready",
                candidate.get("topology_valid") is True,
                not candidate.get("blocker_counts"),
                all(replays[name][field] == original[field] for field in ("node_count", "edge_count", "scope_owned_node_count")),
            )
        )
        direct_ready = not direct_required or all(
            (
                direct_audit.get("status") == "ready",
                direct_audit.get("effect_identity_exact") is True,
                direct_audit.get("operation_shape_exact") is True,
                direct_audit.get("target_e2e_consumed") is False,
            )
        )
        phase_ready = not phase_required or all(
            (
                phase_audit.get("status") == "ready",
                phase_audit.get("effect_identity_exact") is True,
                phase_audit.get("target_e2e_consumed") is False,
            )
        )
        variant_ready[name] = common_ready and direct_ready and phase_ready
    variant_ready["direct"] = variant_ready["direct"] and causal_ready
    ready = all(variant_ready.values())
    effects = causal.get("effects") or []
    model_e2e = int(original.get("simulated_e2e_us") or 0)
    model_scope = int(original.get("simulated_gap_excluded_e2e_us") or 0)
    scopes = {
        name: int(replay.get("simulated_gap_excluded_e2e_us") or 0)
        for name, replay in replays.items()
    }
    target_scope = int(((ledger.get("phase_score") or {}).get("gap_excluded_scope") or {}).get("target_us") or 0)
    return {
        **identity,
        "status": "READY" if ready else "NOT_READY",
        "cpp_replay_count": replay_count,
        "identical_cost_replay_exact": True,
        "errors": [] if ready else ["oracle_structure_or_cost_binding_not_ready"],
        "effect_count": len(effects),
        "cost_response_count": sum(int(row.get("cost_node_completion_response_us") or 0) > 0 for row in effects),
        "variant_ready": variant_ready,
        "direct": score,
        "model_e2e_us": model_e2e,
        "oracle_e2e_us": int(replays["direct"].get("simulated_e2e_us") or 0),
        "model_gap_excluded_scope_us": model_scope,
        "oracle_direct_gap_excluded_scope_us": scopes["direct"],
        "oracle_phase_compute_gap_excluded_scope_us": scopes["phase_compute"],
        "oracle_phase_owner_gap_excluded_scope_us": scopes["phase_owner"],
        "oracle_direct_phase_compute_gap_excluded_scope_us": scopes["direct_phase_compute"],
        "oracle_direct_phase_owner_gap_excluded_scope_us": scopes["direct_phase_owner"],
        "target_gap_excluded_scope_us": target_scope,
    }


def _identity_mismatches(original: dict, replay: dict) -> list[str]:
    """Same costs must reproduce topology counts, scope, timing and critical path."""
    fields = ("node_count", "edge_count", "scope_owned_node_count", "scope_owned_node_duration_us",
              "scope_owned_gap_duration_us", "simulated_e2e_us", "simulated_gap_excluded_e2e_us", "gap_excluded_critical_path")
    return [field for field in fields if original[field] != replay[field]]


def _command(
    runner: dict[str, Any],
    model: Path,
    summary: Path,
    *,
    direct_override: Path | None = None,
    phase_override: Path | None = None,
) -> list[str]:
    cpp = runner["cpp_trace_graph"]
    command = [
        str(trace_graph_executable(runner)),
        "--profile-manifest",
        str(_path(runner["input"]["profile_manifest"])),
        "--run-summary",
        str(summary),
        "--model-config",
        str(model),
    ]
    if direct_override is not None:
        command.extend(("--hicache-oracle-cost-replay", str(direct_override)))
    if phase_override is not None:
        command.extend(("--hicache-phase-oracle-cost-replay", str(phase_override)))
    for name, value in (
        ("--threads", cpp.get("threads")),
        ("--file-threads", cpp.get("file_threads")),
        ("--trace-window-start-us", cpp.get("trace_window_start_us")),
        ("--trace-window-end-us", cpp.get("trace_window_end_us")),
        ("--actual-e2e-us", cpp.get("actual_e2e_us")),
    ):
        if value is not None:
            command.extend((name, str(value)))
    if cpp.get("trace_channels"):
        command.extend(("--trace-channels", ",".join(str(value) for value in cpp["trace_channels"])))
    return command


def _summary(rows: list[dict[str, Any]], expected: int) -> dict[str, Any]:
    ready = [row for row in rows if row.get("status") == "READY"]
    complete = len(rows) == len(ready) == expected
    direct = [row["direct"] for row in ready]
    total = _metrics([(row["predicted_us"], row["actual_us"]) for row in direct], complete)
    delta_error = sum(abs(row["predicted_delta_us"] - row["actual_delta_us"]) for row in direct)
    delta_reference = sum(abs(row["actual_delta_us"]) for row in direct)
    delta_l1 = 100.0 * delta_error / delta_reference if delta_reference else (0.0 if not delta_error else None)
    large = [row for row in direct if row["large_change"]]
    direction = 100.0 * sum(row["direction_correct"] for row in large) / len(large) if large else None
    phase = _metrics(
        [(abs(row["model_e2e_us"] - row["oracle_e2e_us"]), row["direct"]["actual_us"]) for row in ready],
        complete,
        error_is_prediction=True,
    )
    phase_with_direct_oracle_scope = _metrics(
        [(row["oracle_direct_gap_excluded_scope_us"], row["target_gap_excluded_scope_us"]) for row in ready],
        complete and all(row["target_gap_excluded_scope_us"] > 0 for row in ready),
    )
    direct_model_scope_response = _metrics(
        [
            (abs(row["model_gap_excluded_scope_us"] - row["oracle_direct_gap_excluded_scope_us"]), row["target_gap_excluded_scope_us"])
            for row in ready
        ],
        complete and all(row["target_gap_excluded_scope_us"] > 0 for row in ready),
        error_is_prediction=True,
    )
    oracle_cost_scope = _metrics(
        [(row["oracle_direct_phase_compute_gap_excluded_scope_us"], row["target_gap_excluded_scope_us"]) for row in ready],
        complete and all(row["target_gap_excluded_scope_us"] > 0 for row in ready),
    )
    phase_model_scope_response = _metrics(
        [
            (
                abs(row["oracle_direct_gap_excluded_scope_us"] - row["oracle_direct_phase_compute_gap_excluded_scope_us"]),
                row["target_gap_excluded_scope_us"],
            )
            for row in ready
        ],
        complete and all(row["target_gap_excluded_scope_us"] > 0 for row in ready),
        error_is_prediction=True,
    )
    phase_compute_with_direct_model = _metrics(
        [(row["oracle_phase_compute_gap_excluded_scope_us"], row["target_gap_excluded_scope_us"]) for row in ready],
        complete and all(row["target_gap_excluded_scope_us"] > 0 for row in ready),
    )
    phase_owner_with_direct_model = _metrics(
        [(row["oracle_phase_owner_gap_excluded_scope_us"], row["target_gap_excluded_scope_us"]) for row in ready],
        complete and all(row["target_gap_excluded_scope_us"] > 0 for row in ready),
    )
    full_owner_oracle_scope = _metrics(
        [(row["oracle_direct_phase_owner_gap_excluded_scope_us"], row["target_gap_excluded_scope_us"]) for row in ready],
        complete and all(row["target_gap_excluded_scope_us"] > 0 for row in ready),
    )
    phase_control_scope_response = _metrics(
        [
            (
                abs(
                    row["oracle_direct_phase_compute_gap_excluded_scope_us"]
                    - row["oracle_direct_phase_owner_gap_excluded_scope_us"]
                ),
                row["target_gap_excluded_scope_us"],
            )
            for row in ready
        ],
        complete and all(row["target_gap_excluded_scope_us"] > 0 for row in ready),
        error_is_prediction=True,
    )
    effect_count = sum(int(row.get("effect_count") or 0) for row in ready)
    return {
        "status": "OK" if complete else ("PARTIAL" if ready else "ERROR"),
        "diagnostic_only": True,
        "target_scope_used_for_parameters": False,
        "target_scope_used_for_scoring": True,
        "gap_excluded_scope_includes_phase": True,
        "residual_gap_modeled": False,
        "row_count": len(rows),
        "ready_count": len(ready),
        "structure_binding_ready_count": len(ready),
        "causal_effect_count": effect_count,
        "cost_response_count": sum(int(row.get("cost_response_count") or 0) for row in ready),
        "direct_cost": {
            "status": "PASS"
            if total["passed"] and delta_l1 is not None and delta_l1 <= 3.0 and bool(large) and direction == 100.0
            else "FAIL",
            "total": total,
            "delta_weighted_l1_pct": delta_l1,
            "large_change_count": len(large),
            "large_change_direction_accuracy_pct": direction,
        },
        "phase_normalized_e2e": phase,
        "phase_model_with_direct_oracle_gap_excluded_scope": phase_with_direct_oracle_scope,
        "direct_model_gap_excluded_scope_response": direct_model_scope_response,
        "oracle_cost_gap_excluded_scope": oracle_cost_scope,
        "phase_model_gap_excluded_scope_response": phase_model_scope_response,
        "phase_compute_oracle_with_direct_model_gap_excluded_scope": phase_compute_with_direct_model,
        "all_phase_owner_oracle_with_direct_model_gap_excluded_scope": phase_owner_with_direct_model,
        "full_owner_oracle_gap_excluded_scope": full_owner_oracle_scope,
        "phase_cpu_control_gap_excluded_scope_response": phase_control_scope_response,
        "cells": [
            {
                "model_run_id": row["model_run_id"],
                "pair_id": row["pair_id"],
                "status": row["status"],
                "identical_cost_replay_exact": row.get("identical_cost_replay_exact", False),
                "identity_mismatches": row.get("identity_mismatches", []),
                "errors": row.get("errors", []),
                "variant_ready": row.get("variant_ready"),
                "model_scope_us": row.get("model_gap_excluded_scope_us"),
                "direct_oracle_scope_us": row.get("oracle_direct_gap_excluded_scope_us"),
                "phase_compute_oracle_scope_us": row.get("oracle_phase_compute_gap_excluded_scope_us"),
                "phase_owner_oracle_scope_us": row.get("oracle_phase_owner_gap_excluded_scope_us"),
                "direct_phase_compute_oracle_scope_us": row.get("oracle_direct_phase_compute_gap_excluded_scope_us"),
                "full_owner_oracle_scope_us": row.get("oracle_direct_phase_owner_gap_excluded_scope_us"),
                "target_scope_us": row.get("target_gap_excluded_scope_us"),
                "direct_cost": row.get("direct"),
            }
            for row in rows
        ],
        "error_counts": dict(sorted(Counter(error for row in rows for error in row.get("errors", [])).items())),
    }


def _metrics(
    pairs: list[tuple[int, int]],
    complete: bool,
    *,
    error_is_prediction: bool = False,
) -> dict[str, Any]:
    errors = [predicted if error_is_prediction else abs(predicted - actual) for predicted, actual in pairs]
    reference = sum(actual for _, actual in pairs)
    percentages = [100.0 * error / actual for error, (_, actual) in zip(errors, pairs) if actual > 0]
    wape = 100.0 * sum(errors) / reference if reference else (0.0 if not sum(errors) else None)
    p90 = sorted(percentages)[round((len(percentages) - 1) * 0.9)] if percentages else None
    passed = complete and wape is not None and p90 is not None and wape <= 3.0 and p90 <= 5.0
    return {"passed": passed, "count": len(pairs), "wape_pct": wape, "p90_pct": p90}


def _path(value: str | Path) -> Path:
    path = resolve_repo_path(value)
    if path is None:
        raise ValueError(f"missing path: {value}")
    return path
