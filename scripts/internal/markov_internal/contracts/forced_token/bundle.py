"""Minimal input-to-plan mapping for forced-token replay."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .constants import (
    FORCED_TOKEN_ERROR_BUNDLE_INPUT,
    FORCED_TOKEN_ERROR_BUNDLE_MISSING,
    FORCED_TOKEN_ERROR_BUNDLE_PLAN_MISSING,
)
from .plan import load_forced_token_plan, plan_workload_id


@dataclass(frozen=True)
class ForcedTokenBundlePlan:
    input_id: str
    bundle_path: str
    plan_path: str

    def to_dict(self) -> dict[str, Any]:
        return {"path": self.bundle_path, "input_id": self.input_id, "plan_path": self.plan_path}


def load_forced_token_bundle(path: Path) -> dict[str, Any]:
    bundle = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(bundle, dict) or not isinstance(bundle.get("plans"), dict) or not bundle["plans"]:
        raise ValueError("forced token bundle must be an object containing plans")
    return bundle


def resolve_forced_token_bundle_plan(bundle_path: Path, input_id: str) -> ForcedTokenBundlePlan:
    if not bundle_path.is_file():
        raise ValueError(FORCED_TOKEN_ERROR_BUNDLE_MISSING)
    bundle = load_forced_token_bundle(bundle_path)
    entry = bundle["plans"].get(input_id)
    if not isinstance(entry, dict):
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_INPUT}:{input_id}")
    raw_path = entry.get("path")
    if not isinstance(raw_path, str) or not raw_path:
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_PLAN_MISSING}:{input_id}")
    relative = Path(raw_path)
    if relative.is_absolute():
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_PLAN_MISSING}:{input_id}:absolute path")
    bundle_dir = bundle_path.parent.resolve()
    plan_path = (bundle_dir / relative).resolve()
    if bundle_dir not in plan_path.parents or not plan_path.is_file():
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_PLAN_MISSING}:{plan_path}")
    plan = load_forced_token_plan(plan_path)
    if plan_workload_id(plan) != input_id:
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_INPUT}:{input_id}:workload id mismatch")
    return ForcedTokenBundlePlan(input_id, str(bundle_path.resolve()), str(plan_path))


def forced_token_bundle_summary(path: Path | None) -> dict[str, Any]:
    result = {"path": str(path) if path else None, "exists": bool(path and path.is_file()), "input_ids": [], "plan_count": 0, "errors": [], "ready": False}
    if path is None or not path.is_file():
        result["errors"] = [FORCED_TOKEN_ERROR_BUNDLE_MISSING]
        return result
    try:
        plans = load_forced_token_bundle(path)["plans"]
    except (OSError, json.JSONDecodeError, ValueError) as error:
        result["errors"] = [str(error)]
        return result
    result.update({"input_ids": sorted(str(key) for key in plans), "plan_count": len(plans), "ready": True})
    return result
