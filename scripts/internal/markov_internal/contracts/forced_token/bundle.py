"""Minimal input-to-plan mapping for forced-token replay."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .constants import (
    FORCED_TOKEN_ERROR_BUNDLE_INPUT,
    FORCED_TOKEN_ERROR_BUNDLE_MISSING,
    FORCED_TOKEN_ERROR_BUNDLE_PLAN_MISSING,
)
from .plan import load_forced_token_plan, plan_workload_id


def load_forced_token_bundle(path: Path) -> dict[str, Any]:
    bundle = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(bundle, dict) or not isinstance(bundle.get("plans"), dict) or not bundle["plans"]:
        raise ValueError("forced token bundle must be an object containing plans")
    return bundle


def resolve_forced_token_bundle_plan(bundle_path: Path, input_id: str) -> Path:
    """Resolve a workload's validated plan within its bundle directory."""
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
    return plan_path
