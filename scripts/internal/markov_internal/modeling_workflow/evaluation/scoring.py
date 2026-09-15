"""Shared scope/component scoring of already predicted cells; no fitting or execution."""

from __future__ import annotations

from typing import Any

from ..io_model_contract import OPERATION_KINDS
from ..validations.final_dag.shape_compare import compare_shape
from ..validations.hicache.phase.score import PHASE_DELTA_MATERIAL_THRESHOLD_US, compare_phase_work


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


def direct_total(run: dict[str, Any]) -> int:
    """Executed service and intrinsic control; storage retains its declared clock."""

    return sum(sum(observed_direct_cost(row)) for row in run["source_io_observations"]["observations"])


def direct_by_kind(run: dict[str, Any]) -> dict[str, dict[str, int]]:
    result = {kind: {"service_us": 0, "control_us": 0} for kind in OPERATION_KINDS}
    for row in run["source_io_observations"]["observations"]:
        kind = row["kind"]
        service, control = observed_direct_cost(row)
        result[kind]["service_us"] += service
        result[kind]["control_us"] += control
    return result


def score_cell(
    prediction: dict, run: dict, observed: dict, oracle: dict, target_run_id: str, *, include_oracle_costs: bool = False
) -> dict:
    shape = compare_shape(prediction["shape"], oracle)
    phase = compare_phase_work(run, observed, include_oracle_costs=include_oracle_costs)
    totals = prediction["target_predicted"]["totals"]
    predicted, target, base = totals["service_us"] + totals["control_us"], direct_total(observed), direct_total(run)
    source_kinds, target_kinds = direct_by_kind(run), direct_by_kind(observed)
    by_kind = {}
    for kind in OPERATION_KINDS:
        projected = prediction["target_predicted"]["by_kind"][kind]
        predicted_kind = projected["service_us"] + projected["control_us"]
        target_kind = sum(target_kinds[kind].values())
        base_kind = sum(source_kinds[kind].values())
        by_kind[kind] = {
            "predicted_us": predicted_kind,
            "target_us": target_kind,
            "base_us": base_kind,
            "absolute_error_us": abs(predicted_kind - target_kind),
            "ape": abs(predicted_kind - target_kind) / target_kind if target_kind else None,
        }
    component_error = sum(item["absolute_error_us"] for item in by_kind.values())
    return {
        **{key: value for key, value in prediction.items() if key not in {"shape", "target_hicache"}},
        "target_run_id": target_run_id, "target_observation_used": True,
        "structure_exact": shape["acceptance_ready"], "shape_score": shape,
        "phase_structure_exact": phase["structure_exact"], "phase_score": phase,
        "direct": {"predicted_us": predicted, "target_us": target, "base_us": base,
                   "absolute_error_us": abs(predicted - target), "ape": abs(predicted - target) / target if target else None,
                   "component_absolute_error_us": component_error,
                   "component_ape": component_error / target if target else None,
                   "target_delta_us": target - base, "by_kind": by_kind},
    }


def score_metrics(rows: list[dict]) -> dict:
    """Keep the established gates and per-cell percentile units, including missing evidence."""

    scope = _gap_excluded_scope_metrics(rows)
    phase = _phase_cost_metrics(rows)
    delta = _phase_delta_metrics(rows)
    values = [row["direct"] for row in rows]
    apes = sorted(row["ape"] for row in values if row["ape"] is not None)
    total = sum(row["target_us"] for row in values)
    error = sum(row["absolute_error_us"] for row in values)
    component_error = sum(row["component_absolute_error_us"] for row in values)
    component_apes = sorted(row["component_ape"] for row in values if row["component_ape"] is not None)
    change = sum(abs(row["target_delta_us"]) for row in values)
    by_kind = {}
    for kind in OPERATION_KINDS:
        items = [row["by_kind"][kind] for row in values]
        kind_total = sum(item["target_us"] for item in items)
        kind_error = sum(item["absolute_error_us"] for item in items)
        kind_apes = sorted(item["ape"] for item in items if item["ape"] is not None)
        by_kind[kind] = {
            "wape": kind_error / kind_total if kind_total else None,
            "p90_ape": kind_apes[min(len(kind_apes) - 1, int(.9 * len(kind_apes)))] if kind_apes else None,
            "absolute_error_us": kind_error,
            "target_total_us": kind_total,
        }
    direct = {"wape": error / total if total else None,
              "p90_ape": apes[min(len(apes) - 1, int(.9 * len(apes)))] if apes else None,
              "noncancelling_component_wape": component_error / total if total else None,
              "noncancelling_component_p90_ape": component_apes[
                  min(len(component_apes) - 1, int(.9 * len(component_apes)))
              ] if component_apes else None,
              "component_absolute_error_us": component_error,
              "within_cell_component_cancellation_us": component_error - error,
              "delta_weighted_l1": error / change if change else None, "absolute_error_us": error,
              "zero_target_count": len(values) - len(apes), "by_kind": by_kind}

    def within(value, maximum):
        return value is not None and value <= maximum

    gates = {
        "structure": bool(rows) and all(row["status"] == "READY" and row["structure_exact"]
                                        and row["phase_structure_exact"] for row in rows),
        "scope": scope["cell_count"] == len(rows) and within(scope["wape"], .03) and within(scope["p90_ape"], .05),
        "phase": all(row["phase_score"]["cost"]["ready"] for row in rows)
                 and all(phase.get(name, {}).get("cell_count") == len(rows)
                     and within(phase.get(name, {}).get("wape"), .015)
                     and within(phase.get(name, {}).get("cell_p90_ape"), .03)
                     for name in ("prefill_compute", "decode_compute", "combined_compute")),
        "phase_delta": delta["cell_count"] == len(rows) and within(delta["weighted_l1"], .02)
                       and delta["large_change_direction_accuracy"] in (None, 1),
        "direct": direct["zero_target_count"] == 0 and within(direct["wape"], .03)
                  and within(direct["p90_ape"], .05)
                  and within(direct["noncancelling_component_wape"], .03)
                  and within(direct["noncancelling_component_p90_ape"], .05)
                  and within(direct["delta_weighted_l1"], .03),
    }
    return {"status": "PASS" if all(gates.values()) else "MODEL_LIMITATION", "cell_count": len(rows),
            "gates": gates, "scope": scope, "phase": phase, "phase_delta": delta, "direct": direct}


def _phase_cost_metrics(rows: list[dict[str, Any]]) -> dict[str, Any]:
    names = {
        name
        for row in rows
        if row.get("phase_modeled") is True
        for name in ((row.get("phase_score") or {}).get("cost") or {}).get("metrics", {})
    }
    result: dict[str, Any] = {}
    for name in sorted(names):
        metrics = [
            row["phase_score"]["cost"]["metrics"][name]
            for row in rows
            if row.get("phase_modeled") is True and name in row["phase_score"]["cost"]["metrics"]
        ]
        target_total = sum(int(item.get("target_total_us") or 0) for item in metrics)
        weighted_l1 = sum(int(item.get("weighted_l1_us") or 0) for item in metrics)
        p90_values = sorted(float(item["p90_ape"]) for item in metrics if item.get("p90_ape") is not None)
        result[name] = {
            "cell_count": len(metrics),
            "wape": weighted_l1 / max(target_total, 1),
            "cell_p90_ape": p90_values[min(len(p90_values) - 1, int(0.9 * len(p90_values)))] if p90_values else None,
            "weighted_l1_us": weighted_l1,
            "target_total_us": target_total,
        }
    return result

def _gap_excluded_scope_metrics(rows: list[dict[str, Any]]) -> dict[str, Any]:
    values = [
        row["phase_score"]["gap_excluded_scope"]
        for row in rows
        if row.get("phase_modeled") is True
        and (row.get("phase_score") or {}).get("gap_excluded_scope", {}).get("ready") is True
    ]
    target_total = sum(int(item["target_us"]) for item in values)
    absolute = sum(int(item["absolute_error_us"]) for item in values)
    apes = sorted(float(item["ape"]) for item in values)
    return {
        "cell_count": len(values),
        "wape": absolute / max(target_total, 1) if values else None,
        "p90_ape": apes[min(len(apes) - 1, int(0.9 * len(apes)))] if apes else None,
        "weighted_l1_us": absolute if values else None,
        "target_total_us": target_total if values else None,
        "target_opened_after_prediction": True,
    }

def _phase_delta_metrics(rows: list[dict[str, Any]]) -> dict[str, Any]:
    values = [
        row["phase_score"]["cost"]["combined_delta"]
        for row in rows
        if row.get("phase_modeled") is True
        and isinstance(((row.get("phase_score") or {}).get("cost") or {}).get("combined_delta"), dict)
    ]
    denominator = sum(abs(int(item["target_us"])) for item in values)
    absolute = sum(int(item["absolute_error_us"]) for item in values)
    threshold_us = PHASE_DELTA_MATERIAL_THRESHOLD_US
    large = [item for item in values if abs(int(item["target_us"])) > threshold_us]
    return {
        "cell_count": len(values),
        "weighted_l1": absolute / max(denominator, 1) if values else None,
        "absolute_error_us": absolute if values else None,
        "target_delta_l1_us": denominator if values else None,
        "material_threshold_us": threshold_us,
        "large_change_count": len(large),
        "large_change_direction_accuracy": (
            sum(item.get("direction_correct") is True for item in large) / len(large) if large else None
        ),
        "target_cost_opened_after_prediction": True,
    }
