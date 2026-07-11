"""Forced-token plan parsing, identity, summary, and validation."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ...common.digests import sha256_file
from .constants import (
    FORCED_TOKEN_ERROR_REQUEST_COUNT,
    FORCED_TOKEN_ERROR_REQUEST_IDS,
    FORCED_TOKEN_ERROR_REQUEST_TOKENS,
    FORCED_TOKEN_ERROR_SCHEMA_UNKNOWN,
    FORCED_TOKEN_ERROR_WORKLOAD_FINGERPRINT,
    FORCED_TOKEN_ERROR_WORKLOAD_ID,
    FORCED_TOKEN_PLAN_SCHEMA,
)


@dataclass(frozen=True)
class ForcedTokenPlanSummary:
    """Compact plan metadata persisted by manifests and preflight reports."""

    path: str | None
    exists: bool
    schema: str | None
    sha256: str | None
    workload_id: str | None
    workload_fingerprint: str | None
    request_count: int
    capture: dict[str, Any]

    def to_dict(self) -> dict[str, Any]:
        """Return the stable JSON representation of this summary."""

        return {
            "path": self.path,
            "exists": self.exists,
            "schema": self.schema,
            "sha256": self.sha256,
            "workload_id": self.workload_id,
            "workload_fingerprint": self.workload_fingerprint,
            "request_count": self.request_count,
            "capture": self.capture,
        }


def non_negative_int(value: Any) -> int:
    """Parse report counters conservatively, returning zero for invalid input."""

    if value is None or isinstance(value, bool):
        return 0
    try:
        return max(0, int(float(value)))
    except (TypeError, ValueError):
        return 0


def int_list(value: Any) -> list[int] | None:
    """Return an integer list only when every JSON item is a non-boolean int."""

    if not isinstance(value, list):
        return None
    parsed: list[int] = []
    for item in value:
        if not isinstance(item, int) or isinstance(item, bool):
            return None
        parsed.append(item)
    return parsed


def load_forced_token_plan(path: Path) -> dict[str, Any]:
    """Load a plan and enforce the minimum active schema contract."""

    plan = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(plan, dict):
        raise ValueError("forced token plan must be a JSON object")
    if plan.get("schema") != FORCED_TOKEN_PLAN_SCHEMA:
        raise ValueError(f"unsupported forced token plan schema: {plan.get('schema')}")
    if not isinstance(plan.get("requests"), list):
        raise ValueError("forced token plan must contain a requests list")
    return plan


def plan_workload_id(plan: dict[str, Any]) -> str | None:
    """Return the non-empty workload identifier declared by a plan."""

    value = plan.get("workload_id")
    return value if isinstance(value, str) and value else None


def plan_workload_fingerprint(plan: dict[str, Any]) -> str | None:
    """Return the non-empty workload fingerprint declared by a plan."""

    value = plan.get("workload_fingerprint")
    return value if isinstance(value, str) and value else None


def plan_request_count(plan: dict[str, Any]) -> int:
    """Return the number of request entries in a loaded plan."""

    requests = plan.get("requests")
    return len(requests) if isinstance(requests, list) else 0


def index_plan_requests_by_logical_id(plan: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """Index plan requests by unique, non-empty logical request identifier."""

    indexed: dict[str, dict[str, Any]] = {}
    for request in plan.get("requests") or []:
        if not isinstance(request, dict):
            raise ValueError("forced token plan contains a non-object request")
        logical_id = request.get("logical_request_id")
        if not isinstance(logical_id, str) or not logical_id:
            raise ValueError("forced token plan request missing logical_request_id")
        if logical_id in indexed:
            raise ValueError(f"duplicate logical_request_id in forced token plan: {logical_id}")
        indexed[logical_id] = request
    return indexed


def forced_token_plan_summary(path: Path | None) -> ForcedTokenPlanSummary:
    """Read non-throwing plan metadata for preflight and quality reporting."""

    if path is None:
        return _missing_plan_summary(None)
    if not path.is_file():
        return _missing_plan_summary(str(path))
    try:
        plan = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return ForcedTokenPlanSummary(
            path=str(path),
            exists=True,
            schema=None,
            sha256=sha256_file(path),
            workload_id=None,
            workload_fingerprint=None,
            request_count=0,
            capture={},
        )
    if not isinstance(plan, dict):
        plan = {}
    capture = plan.get("capture") if isinstance(plan.get("capture"), dict) else {}
    return ForcedTokenPlanSummary(
        path=str(path),
        exists=True,
        schema=str(plan.get("schema")) if plan.get("schema") is not None else None,
        sha256=sha256_file(path),
        workload_id=plan_workload_id(plan),
        workload_fingerprint=plan_workload_fingerprint(plan),
        request_count=plan_request_count(plan),
        capture=capture,
    )


def _missing_plan_summary(path: str | None) -> ForcedTokenPlanSummary:
    """Construct the canonical summary for an absent plan."""

    return ForcedTokenPlanSummary(
        path=path,
        exists=False,
        schema=None,
        sha256=None,
        workload_id=None,
        workload_fingerprint=None,
        request_count=0,
        capture={},
    )


def validate_plan_contract(
    plan: dict[str, Any],
    *,
    workload_id: str | None,
    workload_fingerprint: str | None,
    expected_request_ids: list[str] | None = None,
) -> list[str]:
    """Validate that a plan describes the current logical workload timeline."""

    errors: list[str] = []
    if plan.get("schema") != FORCED_TOKEN_PLAN_SCHEMA:
        errors.append(FORCED_TOKEN_ERROR_SCHEMA_UNKNOWN)
    if workload_id and plan_workload_id(plan) != workload_id:
        errors.append(FORCED_TOKEN_ERROR_WORKLOAD_ID)
    if workload_fingerprint and plan_workload_fingerprint(plan) != workload_fingerprint:
        errors.append(FORCED_TOKEN_ERROR_WORKLOAD_FINGERPRINT)

    requests = plan.get("requests")
    if not isinstance(requests, list) or not requests:
        errors.append(FORCED_TOKEN_ERROR_REQUEST_COUNT)
    for request in requests or []:
        if not isinstance(request, dict):
            errors.append(FORCED_TOKEN_ERROR_REQUEST_COUNT)
            continue
        if int_list(request.get("origin_input_ids")) is None or int_list(request.get("forced_output_ids")) is None:
            errors.append(FORCED_TOKEN_ERROR_REQUEST_TOKENS)
    if expected_request_ids is not None:
        actual_request_ids = [
            str(request.get("logical_request_id")) for request in requests or [] if isinstance(request, dict)
        ]
        if actual_request_ids != expected_request_ids:
            errors.append(FORCED_TOKEN_ERROR_REQUEST_IDS)
    return sorted(set(errors))
