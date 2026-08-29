"""Minimal forced-token request-sequence contract."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .constants import (
    FORCED_TOKEN_ERROR_REQUEST_COUNT,
    FORCED_TOKEN_ERROR_REQUEST_IDS,
    FORCED_TOKEN_ERROR_REQUEST_TOKENS,
    FORCED_TOKEN_ERROR_WORKLOAD_ID,
)


@dataclass(frozen=True)
class ForcedTokenPlanSummary:
    path: str | None
    exists: bool
    workload_id: str | None
    request_count: int

    def to_dict(self) -> dict[str, Any]:
        return {
            "path": self.path,
            "exists": self.exists,
            "workload_id": self.workload_id,
            "request_count": self.request_count,
        }


def non_negative_int(value: Any) -> int:
    if value is None or isinstance(value, bool):
        return 0
    try:
        return max(0, int(float(value)))
    except (TypeError, ValueError):
        return 0


def int_list(value: Any) -> list[int] | None:
    if not isinstance(value, list) or any(not isinstance(item, int) or isinstance(item, bool) for item in value):
        return None
    return list(value)


def load_forced_token_plan(path: Path) -> dict[str, Any]:
    plan = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(plan, dict) or not isinstance(plan.get("requests"), list):
        raise ValueError("forced token plan must be an object containing requests")
    return plan


def plan_workload_id(plan: dict[str, Any]) -> str | None:
    value = plan.get("workload_id")
    return value if isinstance(value, str) and value else None


def plan_request_count(plan: dict[str, Any]) -> int:
    requests = plan.get("requests")
    return len(requests) if isinstance(requests, list) else 0


def index_plan_requests_by_logical_id(plan: dict[str, Any]) -> dict[str, dict[str, Any]]:
    indexed: dict[str, dict[str, Any]] = {}
    for request in plan.get("requests") or []:
        if not isinstance(request, dict):
            raise ValueError("forced token plan contains a non-object request")
        logical_id = request.get("logical_request_id")
        if not isinstance(logical_id, str) or not logical_id or logical_id in indexed:
            raise ValueError(f"invalid or duplicate forced-token request id: {logical_id}")
        indexed[logical_id] = request
    return indexed


def forced_token_plan_summary(path: Path | None) -> ForcedTokenPlanSummary:
    if path is None or not path.is_file():
        return ForcedTokenPlanSummary(str(path) if path else None, False, None, 0)
    try:
        plan = load_forced_token_plan(path)
    except (OSError, json.JSONDecodeError, ValueError):
        return ForcedTokenPlanSummary(str(path), True, None, 0)
    return ForcedTokenPlanSummary(str(path), True, plan_workload_id(plan), plan_request_count(plan))


def validate_plan_contract(
    plan: dict[str, Any],
    *,
    workload_id: str | None,
    expected_request_ids: list[str] | None = None,
) -> list[str]:
    errors: list[str] = []
    if workload_id and plan_workload_id(plan) != workload_id:
        errors.append(FORCED_TOKEN_ERROR_WORKLOAD_ID)
    requests = plan.get("requests")
    if not isinstance(requests, list) or not requests:
        errors.append(FORCED_TOKEN_ERROR_REQUEST_COUNT)
    for request in requests or []:
        if not isinstance(request, dict):
            errors.append(FORCED_TOKEN_ERROR_REQUEST_COUNT)
        elif int_list(request.get("origin_input_ids")) is None or int_list(request.get("forced_output_ids")) is None:
            errors.append(FORCED_TOKEN_ERROR_REQUEST_TOKENS)
    if expected_request_ids is not None:
        actual = [str(row.get("logical_request_id")) for row in requests or [] if isinstance(row, dict)]
        if actual != expected_request_ids:
            errors.append(FORCED_TOKEN_ERROR_REQUEST_IDS)
    return sorted(set(errors))
