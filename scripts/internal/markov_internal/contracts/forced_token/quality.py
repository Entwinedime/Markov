"""Quality gates derived from forced-token workload reports."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .constants import (
    FORCED_TOKEN_BUNDLE_SCHEMA,
    FORCED_TOKEN_ERROR_BUNDLE_PROVENANCE,
    FORCED_TOKEN_ERROR_MODE_UNKNOWN,
    FORCED_TOKEN_ERROR_ORIGIN_INPUT,
    FORCED_TOKEN_ERROR_OUTPUT_MISMATCH,
    FORCED_TOKEN_ERROR_OUTPUT_UNCHECKED,
    FORCED_TOKEN_ERROR_PLAN_MISSING,
    FORCED_TOKEN_ERROR_REQUEST_COUNT,
    FORCED_TOKEN_ERROR_SCHEMA_UNKNOWN,
    FORCED_TOKEN_PLAN_SCHEMA,
)
from .plan import non_negative_int


def empty_forced_token_quality() -> dict[str, Any]:
    """Return the stable pass-through result for workloads without forced tokens."""

    return {
        "enabled": False,
        "mode": "none",
        "ready": True,
        "plan_ready": True,
        "bundle_ready": True,
        "errors": [],
        "plan_schema": None,
        "plan_sha256": None,
        "plan_workload_id": None,
        "plan_workload_fingerprint": None,
        "plan_capture": None,
        "plan_capture_run_id": None,
        "plan_capture_config_id": None,
        "plan_capture_input_id": None,
        "plan_capture_model_path": None,
        "plan_capture_tokenizer_digest": None,
        "request_count": 0,
        "output_checked_count": 0,
        "mismatch_count": 0,
        "unchecked_count": 0,
        "prompt_mismatch_count": 0,
        "all_actual_outputs_match_plan": None,
        "plan_written": None,
        "bundle_path": None,
        "bundle_schema": None,
        "bundle_sha256": None,
        "bundle_id": None,
        "bundle_plan_sha256": None,
    }


def forced_token_quality_from_workload_report(path: Path | None) -> dict[str, Any]:
    """Load a workload report and evaluate its forced-token contract."""

    result = empty_forced_token_quality()
    if path is None or not path.is_file():
        return result
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return result
    return forced_token_quality_from_report(report) if isinstance(report, dict) else result


def forced_token_quality_from_report(report: dict[str, Any]) -> dict[str, Any]:
    """Evaluate forced-token readiness from an already loaded workload report."""

    result = empty_forced_token_quality()
    forced = report.get("forced_token")
    if not isinstance(forced, dict) or not forced.get("enabled"):
        return result

    mode = str(forced.get("mode") or "unknown")
    errors: list[str] = []
    result.update(
        {
            "enabled": True,
            "mode": mode,
            "ready": bool(forced.get("ready", False)),
            "plan_schema": forced.get("plan_schema"),
            "plan_sha256": forced.get("plan_sha256"),
            "plan_workload_id": forced.get("plan_workload_id"),
            "plan_workload_fingerprint": forced.get("plan_workload_fingerprint"),
            "plan_capture": forced.get("plan_capture") if isinstance(forced.get("plan_capture"), dict) else None,
            "plan_capture_run_id": forced.get("plan_capture_run_id"),
            "plan_capture_config_id": forced.get("plan_capture_config_id"),
            "plan_capture_input_id": forced.get("plan_capture_input_id"),
            "plan_capture_model_path": forced.get("plan_capture_model_path"),
            "plan_capture_tokenizer_digest": forced.get("plan_capture_tokenizer_digest"),
            "request_count": non_negative_int(forced.get("request_count")),
            "output_checked_count": non_negative_int(forced.get("output_checked_count")),
            "mismatch_count": non_negative_int(forced.get("mismatch_count")),
            "unchecked_count": non_negative_int(forced.get("unchecked_count")),
            "prompt_mismatch_count": non_negative_int(forced.get("prompt_mismatch_count")),
            "all_actual_outputs_match_plan": forced.get("all_actual_outputs_match_plan"),
            "plan_written": forced.get("plan_written"),
            "bundle_path": forced.get("bundle_path"),
            "bundle_schema": forced.get("bundle_schema"),
            "bundle_sha256": forced.get("bundle_sha256"),
            "bundle_id": forced.get("bundle_id"),
            "bundle_plan_sha256": forced.get("bundle_plan_sha256"),
        }
    )

    errors.extend(_forced_token_contract_errors(forced, result, mode))

    result["errors"] = sorted(set(errors))
    result["plan_ready"] = not any(error != FORCED_TOKEN_ERROR_BUNDLE_PROVENANCE for error in result["errors"])
    result["bundle_ready"] = mode != "replay" or FORCED_TOKEN_ERROR_BUNDLE_PROVENANCE not in result["errors"]
    result["ready"] = result["plan_ready"] and result["bundle_ready"]
    return result


def _forced_token_contract_errors(forced: dict[str, Any], result: dict[str, Any], mode: str) -> list[str]:
    """Evaluate mode-independent and mode-specific forced-token invariants."""

    errors = _forced_token_plan_errors(forced, result)
    if mode == "capture":
        if forced.get("plan_written") is not True:
            errors.append(FORCED_TOKEN_ERROR_PLAN_MISSING)
    elif mode == "replay":
        errors.extend(_forced_token_replay_errors(forced, result))
    else:
        errors.append(FORCED_TOKEN_ERROR_MODE_UNKNOWN)
    return errors


def _forced_token_plan_errors(forced: dict[str, Any], result: dict[str, Any]) -> list[str]:
    """Evaluate plan identity fields shared by capture and replay modes."""

    errors: list[str] = []
    if forced.get("plan_schema") != FORCED_TOKEN_PLAN_SCHEMA:
        errors.append(FORCED_TOKEN_ERROR_SCHEMA_UNKNOWN)
    if not forced.get("plan_sha256"):
        errors.append(FORCED_TOKEN_ERROR_PLAN_MISSING)
    if result["request_count"] <= 0:
        errors.append(FORCED_TOKEN_ERROR_REQUEST_COUNT)
    return errors


def _forced_token_replay_errors(forced: dict[str, Any], result: dict[str, Any]) -> list[str]:
    """Evaluate replay output exactness and bundle provenance."""

    errors: list[str] = []
    if forced.get("all_actual_outputs_match_plan") is not True:
        errors.append(FORCED_TOKEN_ERROR_OUTPUT_MISMATCH)
    if result["unchecked_count"] > 0 or result["output_checked_count"] != result["request_count"]:
        errors.append(FORCED_TOKEN_ERROR_OUTPUT_UNCHECKED)
    if result["prompt_mismatch_count"] > 0:
        errors.append(FORCED_TOKEN_ERROR_ORIGIN_INPUT)
    if (
        not forced.get("bundle_path")
        or forced.get("bundle_schema") != FORCED_TOKEN_BUNDLE_SCHEMA
        or not forced.get("bundle_sha256")
        or not forced.get("bundle_id")
        or forced.get("bundle_plan_sha256") != forced.get("plan_sha256")
    ):
        errors.append(FORCED_TOKEN_ERROR_BUNDLE_PROVENANCE)
    return errors
