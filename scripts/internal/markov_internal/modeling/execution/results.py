"""Formal source-only HiCache prediction orchestration."""

from __future__ import annotations

from collections import Counter
import math
from typing import Any

from markov_internal.common.io import load_json
from markov_internal.common.paths import repo_relative_path
from markov_internal.modeling.types import ModelRunResult


def read_row(result: ModelRunResult) -> dict[str, Any]:
    """Read one completed attempt; failed or unstarted work never reads old output."""
    report = result.spec.output_dir / "run_summary.json"
    run = load_json(report) if result.ok and report.is_file() else {}
    return build_row(result, run)


def build_row(result: ModelRunResult, run_summary: dict[str, Any]) -> dict[str, Any]:
    """Bind source/target identity to execution, not structure or accuracy acceptance."""

    execution = run_summary.get("module_results", {}).get("hicache_execution", {})
    spec = result.spec
    # Skipped or failed commands have their own cause; no execution report is
    # expected. Check report integrity only after a successful command.
    blockers = execution_blockers(execution) if result.ok else []
    if result.skip_reason:
        blockers.append(result.skip_reason)
    if result.return_code != 0:
        if result.structure_gaps:
            blockers.append("structure_limitation")
        elif result.missing_costs:
            blockers.append("cost_data_missing")
        else:
            blockers.append("model_command_failed")

    if result.structure_gaps:
        next_action = "检查源语义和目标执行规则；成本补采不能解决结构缺口。"
    elif result.missing_costs:
        next_action = "先核对独立性能库和模型定义；缺测时安排有预算、整组共享的独立采样。"
    elif result.return_code != 0:
        next_action = "查看本次失败日志，确定输入、环境或执行问题；此目标没有有效预测时间。"
    elif blockers:
        next_action = "先完成输入准备和执行；未执行或未完成的结果不能用于选配置。"
    else:
        next_action = "可比较预测时间；先查看成本范围和保留等待，预测排序尚未经过实测确认。"

    return {
        "model_run_id": spec.run_id,
        "pair_id": spec.label,
        "workload_id": spec.source.input_id,
        "source_run_id": spec.source.run_id,
        "source_manifest": str(spec.source.manifest_path),
        "source_replay_report": str(repo_relative_path(run_summary["source_replay_report"]))
        if run_summary.get("source_replay_report")
        else None,
        "source_config_id": spec.source.config_id,
        "target_config": spec.target.label,
        "target_hicache": dict(spec.target.fields),
        "is_self": spec.target.matches_source(spec.source),
        "status": "NOT_READY" if blockers else "EXECUTED",
        "blockers": blockers,
        "next_action": next_action,
        "failure_log": str(repo_relative_path(spec.output_dir / "model.log")) if result.return_code != 0 else None,
        "http_e2e_us": execution.get("http_us") if not blockers else None,
        "cost_coverage": execution.get("cost_coverage", "unknown"),
        "phase_operator_costs": execution.get("phase_operator_costs"),
        "independent_operator_queries": execution.get("independent_operator_queries"),
        "executed_cost_sources": execution.get("executed_cost_sources"),
        "approximations": execution.get("remaining_approximations", []),
        "missing_costs": list(result.missing_costs),
        "structure_gaps": list(result.structure_gaps),
        "performance": {**run_summary.get("performance", {}), **result.performance},
    }


def summarize(rows: list[dict[str, Any]]) -> dict[str, Any]:
    completed_count = sum(row["status"] == "EXECUTED" for row in rows)
    blockers: Counter[str] = Counter()
    for row in rows:
        blockers.update(row["blockers"])
    return {
        "status": "EXECUTED" if rows and completed_count == len(rows) else "CHECK",
        "mode": "prediction",
        "dag_model_count": len(rows),
        "completed_count": completed_count,
        "blocker_counts": dict(sorted(blockers.items())),
        "residual_gap_modeled": False,
        "cost_coverage_counts": dict(sorted(Counter(row["cost_coverage"] for row in rows).items())),
        "missing_costs": shared_cost_gaps(rows),
        "structure_gaps": [
            {"model_run_id": row["model_run_id"], **gap} for row in rows for gap in row["structure_gaps"]
        ],
        "cost_requirements_complete": completed_count == len(rows) and bool(rows),
        "cells": rows,
        "configuration_comparison": configuration_comparison(rows),
    }


def shared_cost_gaps(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """One demand per operation for the group; retain all observed work coordinates.

    These are first encountered gaps, not a complete inventory or permission to
    collect. The acquisition planner must still establish a suitable experiment.
    """
    gaps = {}
    for row in rows:
        for need in row["missing_costs"]:
            gap = gaps.setdefault(
                need["component"], dict(component=need["component"], reasons=[], coordinates=[], cells=[])
            )
            if need["reason"] not in gap["reasons"]:
                gap["reasons"].append(need["reason"])
            if need["coordinates"] not in gap["coordinates"]:
                gap["coordinates"].append(need["coordinates"])
            if row["model_run_id"] not in gap["cells"]:
                gap["cells"].append(row["model_run_id"])
    return list(gaps.values())


def execution_blockers(execution: dict[str, Any]) -> list[str]:
    """Execution integrity, independent of target observations or cost accuracy."""

    if not execution:
        return ["execution_result_missing"]
    blockers = []
    if (
        execution.get("status") != "executed"
        or not execution.get("prepared_facts")
        or execution.get("consumed_facts") != execution.get("prepared_facts")
    ):
        blockers.append("state_execution_incomplete")
    if execution.get("confirmations", {}).get("partial_window_rounds") != 0:
        blockers.append("confirmation_window_incomplete")
    duration = execution.get("http_us")
    if duration is None or not math.isfinite(duration) or duration <= 0:
        blockers.append("http_window_unavailable")
    return blockers


def configuration_comparison(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Order predictions within one source/workload, without target observations.

    A self prediction is the only base-time reference here. Its absence remains
    missing: profiled source wall time is not a normal base measurement. Partial
    candidate sets have an order for inspection, but no asserted fastest target.
    """

    groups: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for row in rows:
        groups.setdefault((row["source_manifest"], row["workload_id"]), []).append(row)

    comparisons = []
    for (manifest, workload), group in groups.items():
        executed = [row for row in group if row["status"] == "EXECUTED"]
        ordered = sorted(executed, key=lambda row: row["http_e2e_us"])
        complete = len(executed) == len(group)
        base = next((row for row in executed if row["is_self"]), None)
        base_us = base["http_e2e_us"] if base is not None else None

        comparisons.append(
            {
                "source_manifest": manifest,
                "source_config_id": group[0]["source_config_id"],
                "workload_id": workload,
                "status": "predicted_order" if complete else "incomplete_candidates",
                "interpretation": "预测排序，不是已验证推荐，也不是统计置信区间。",
                "base_reference": {"kind": "self_prediction", "http_e2e_us": base_us} if base is not None else None,
                "predicted_fastest_targets": [
                    row["target_config"]
                    for row in ordered
                    if complete and row["http_e2e_us"] == ordered[0]["http_e2e_us"]
                ]
                if complete
                else None,
                "ordered_targets": [
                    {
                        "target_config": row["target_config"],
                        "http_e2e_us": row["http_e2e_us"],
                        "speedup_vs_base": base_us / row["http_e2e_us"] if base_us is not None else None,
                        "time_reduction_vs_base": 1 - row["http_e2e_us"] / base_us if base_us is not None else None,
                        "model_run_id": row["model_run_id"],
                    }
                    for row in ordered
                ],
                "unpredicted_targets": [row["target_config"] for row in group if row["status"] != "EXECUTED"],
            }
        )

    return comparisons
