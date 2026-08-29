"""Forced-token readiness derived from actual request outcomes."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .constants import (
    FORCED_TOKEN_ERROR_MODE_UNKNOWN,
    FORCED_TOKEN_ERROR_ORIGIN_INPUT,
    FORCED_TOKEN_ERROR_OUTPUT_MISMATCH,
    FORCED_TOKEN_ERROR_OUTPUT_UNCHECKED,
    FORCED_TOKEN_ERROR_PLAN_MISSING,
    FORCED_TOKEN_ERROR_REQUEST_COUNT,
)
from .plan import non_negative_int


def empty_forced_token_quality() -> dict[str, Any]:
    return {
        "enabled": False,
        "mode": "none",
        "ready": True,
        "errors": [],
        "plan_workload_id": None,
        "request_count": 0,
        "output_checked_count": 0,
        "mismatch_count": 0,
        "unchecked_count": 0,
        "prompt_mismatch_count": 0,
        "all_actual_outputs_match_plan": None,
        "plan_written": None,
    }


def forced_token_quality_from_workload_report(path: Path | None) -> dict[str, Any]:
    if path is None or not path.is_file():
        return empty_forced_token_quality()
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return empty_forced_token_quality()
    return forced_token_quality_from_report(report) if isinstance(report, dict) else empty_forced_token_quality()


def forced_token_quality_from_report(report: dict[str, Any]) -> dict[str, Any]:
    result = empty_forced_token_quality()
    forced = report.get("forced_token")
    if not isinstance(forced, dict) or not forced.get("enabled"):
        return result
    mode = str(forced.get("mode") or "unknown")
    result.update(
        {
            "enabled": True,
            "mode": mode,
            "plan_workload_id": forced.get("plan_workload_id"),
            "request_count": non_negative_int(forced.get("request_count")),
            "output_checked_count": non_negative_int(forced.get("output_checked_count")),
            "mismatch_count": non_negative_int(forced.get("mismatch_count")),
            "unchecked_count": non_negative_int(forced.get("unchecked_count")),
            "prompt_mismatch_count": non_negative_int(forced.get("prompt_mismatch_count")),
            "all_actual_outputs_match_plan": forced.get("all_actual_outputs_match_plan"),
            "plan_written": forced.get("plan_written"),
        }
    )
    errors: list[str] = []
    if result["request_count"] <= 0:
        errors.append(FORCED_TOKEN_ERROR_REQUEST_COUNT)
    if mode == "capture":
        if result["plan_written"] is not True:
            errors.append(FORCED_TOKEN_ERROR_PLAN_MISSING)
    elif mode == "replay":
        if result["all_actual_outputs_match_plan"] is not True:
            errors.append(FORCED_TOKEN_ERROR_OUTPUT_MISMATCH)
        if result["unchecked_count"] or result["output_checked_count"] != result["request_count"]:
            errors.append(FORCED_TOKEN_ERROR_OUTPUT_UNCHECKED)
        if result["prompt_mismatch_count"]:
            errors.append(FORCED_TOKEN_ERROR_ORIGIN_INPUT)
    else:
        errors.append(FORCED_TOKEN_ERROR_MODE_UNKNOWN)
    result["errors"] = sorted(set(errors))
    result["ready"] = not errors
    return result
