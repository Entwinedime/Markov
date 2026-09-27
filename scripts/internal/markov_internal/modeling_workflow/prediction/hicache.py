"""Formal source-only HiCache prediction orchestration."""

from __future__ import annotations

from collections import Counter
from typing import Any

from ...common.io import load_json
from ..types import ModelRunResult


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
        blockers.append("model_command_failed")
    return {
        "model_run_id": spec.run_id,
        "pair_id": spec.label,
        "workload_id": spec.source.input_id,
        "source_run_id": spec.source.run_id,
        "source_manifest": str(spec.source.manifest_path),
        "source_config_id": spec.source.config_id,
        "target_config": spec.target.label,
        "target_hicache": dict(spec.target.fields),
        "is_self": spec.target.matches_source(spec.source),
        "status": "NOT_READY" if blockers else "EXECUTED",
        "blockers": blockers,
        "http_e2e_us": execution.get("http_us") if not blockers else None,
        "cost_coverage": execution.get("cost_coverage", "unknown"),
        "approximations": execution.get("remaining_approximations", []),
        "missing_costs": list(result.missing_costs),
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
        "cost_requirements_complete": completed_count == len(rows) and bool(rows),
        "cells": rows,
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
    return blockers
