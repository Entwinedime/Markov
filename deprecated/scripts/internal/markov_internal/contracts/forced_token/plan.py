"""Minimal forced-token request-sequence contract."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .constants import (
    FORCED_TOKEN_ERROR_REQUEST_COUNT,
    FORCED_TOKEN_ERROR_REQUEST_IDS,
    FORCED_TOKEN_ERROR_REQUEST_TOKENS,
    FORCED_TOKEN_ERROR_WORKLOAD_ID,
)


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


def validate_plan_contract(
    plan: dict[str, Any],
    *,
    workload_id: str | None,
    expected_request_ids: list[str] | None = None,
) -> list[str]:
    """Validate identities and token arrays after loading the plan's root structure."""

    errors: list[str] = []
    if workload_id and plan_workload_id(plan) != workload_id:
        errors.append(FORCED_TOKEN_ERROR_WORKLOAD_ID)
    requests = plan["requests"]
    if not requests:
        errors.append(FORCED_TOKEN_ERROR_REQUEST_COUNT)
    request_ids: list[str] = []
    for request in requests:
        if not isinstance(request, dict):
            errors.append(FORCED_TOKEN_ERROR_REQUEST_COUNT)
            continue
        logical_id = request.get("logical_request_id")
        if not isinstance(logical_id, str) or not logical_id or logical_id in request_ids:
            errors.append(FORCED_TOKEN_ERROR_REQUEST_IDS)
        else:
            request_ids.append(logical_id)
        if int_list(request.get("origin_input_ids")) is None or int_list(request.get("forced_output_ids")) is None:
            errors.append(FORCED_TOKEN_ERROR_REQUEST_TOKENS)
    if expected_request_ids is not None and request_ids != expected_request_ids:
        errors.append(FORCED_TOKEN_ERROR_REQUEST_IDS)
    return sorted(set(errors))
