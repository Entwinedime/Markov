"""Minimal forced-token request-sequence contract."""

from __future__ import annotations

import json
from collections.abc import Mapping
from pathlib import Path
from typing import Any

from .constants import (
    FORCED_TOKEN_ERROR_REQUEST_COUNT,
    FORCED_TOKEN_ERROR_REQUEST_IDS,
    FORCED_TOKEN_ERROR_REQUEST_TOKENS,
    FORCED_TOKEN_ERROR_FINISH_REASON,
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


def output_work_error(
    output_ids: list[int], max_new_tokens: int, ignore_eos: bool, finish_reason: Mapping[str, Any]
) -> str | None:
    """Check measured work and its native end condition, not the generated text.

    The limit is a reservation bound. EOS may produce fewer tokens, but its
    matched token must be the final recorded token. Only EOS and length-limited
    generation are supported by the serial recording/replay contract.
    """

    count = len(output_ids)
    if count == 0 or count > max_new_tokens:
        return "output_count_outside_limit"

    end_type = finish_reason.get("type")
    if end_type == "length":
        if count != max_new_tokens or finish_reason.get("length") != max_new_tokens:
            return "length_end_does_not_match_work"
        return None

    if end_type == "stop":
        if ignore_eos or count >= max_new_tokens or finish_reason.get("matched") != output_ids[-1]:
            return "eos_end_does_not_match_work"
        return None

    return "unsupported_finish_reason"


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

        output_fields = {"max_new_tokens", "ignore_eos", "finish_reason"}
        if output_fields.intersection(request):
            limit = request.get("max_new_tokens")
            ignore_eos = request.get("ignore_eos")
            reason = request.get("finish_reason")
            if (
                not isinstance(limit, int)
                or isinstance(limit, bool)
                or limit <= 0
                or not isinstance(ignore_eos, bool)
                or not isinstance(reason, dict)
            ):
                errors.append(FORCED_TOKEN_ERROR_FINISH_REASON)
            elif (tokens := int_list(request.get("forced_output_ids"))) is not None:
                if output_work_error(tokens, limit, ignore_eos, reason):
                    errors.append(FORCED_TOKEN_ERROR_FINISH_REASON)
    if expected_request_ids is not None and request_ids != expected_request_ids:
        errors.append(FORCED_TOKEN_ERROR_REQUEST_IDS)
    return sorted(set(errors))
