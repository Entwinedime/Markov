"""Forced-token bundle loading, provenance checks, and summaries."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ...common.digests import sha256_file
from .constants import (
    FORCED_TOKEN_BUNDLE_SCHEMA,
    FORCED_TOKEN_ERROR_BUNDLE_INPUT,
    FORCED_TOKEN_ERROR_BUNDLE_MISSING,
    FORCED_TOKEN_ERROR_BUNDLE_PLAN_HASH,
    FORCED_TOKEN_ERROR_BUNDLE_PLAN_METADATA,
    FORCED_TOKEN_ERROR_BUNDLE_PLAN_MISSING,
    FORCED_TOKEN_ERROR_BUNDLE_SCHEMA,
)
from .plan import (
    load_forced_token_plan,
    non_negative_int,
    plan_request_count,
    plan_workload_fingerprint,
    plan_workload_id,
)


@dataclass(frozen=True)
class ForcedTokenBundlePlan:
    """Validated provenance for one input's plan within a suite bundle."""

    input_id: str
    bundle_path: str
    bundle_sha256: str
    bundle_id: str | None
    plan_path: str
    plan_sha256: str
    entry: dict[str, Any]

    def to_dict(self) -> dict[str, Any]:
        """Return provenance suitable for runner configs and quality reports."""

        return {
            "schema": FORCED_TOKEN_BUNDLE_SCHEMA,
            "path": self.bundle_path,
            "sha256": self.bundle_sha256,
            "bundle_id": self.bundle_id,
            "input_id": self.input_id,
            "plan_path": self.plan_path,
            "plan_sha256": self.plan_sha256,
        }


def load_forced_token_bundle(path: Path) -> dict[str, Any]:
    """Load a bundle and enforce every required suite-level identity field."""

    bundle = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(bundle, dict):
        raise ValueError("forced token bundle must be a JSON object")
    if bundle.get("schema") != FORCED_TOKEN_BUNDLE_SCHEMA:
        raise ValueError(f"unsupported forced token bundle schema: {bundle.get('schema')}")
    for field in ("bundle_id", "capture_suite_dir", "capture_config", "model_path", "server_config_id"):
        if not isinstance(bundle.get(field), str) or not bundle[field]:
            raise ValueError(f"forced token bundle must contain {field}")
    if not isinstance(bundle.get("plans"), dict) or not bundle["plans"]:
        raise ValueError("forced token bundle must contain a non-empty plans object")
    return bundle


def resolve_forced_token_bundle_plan(bundle_path: Path, input_id: str) -> ForcedTokenBundlePlan:
    """Resolve and validate one input plan without allowing path escape."""

    if not bundle_path.is_file():
        raise ValueError(FORCED_TOKEN_ERROR_BUNDLE_MISSING)
    try:
        bundle = load_forced_token_bundle(bundle_path)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_SCHEMA}:{error}") from error

    raw_entry = bundle["plans"].get(input_id)
    if not isinstance(raw_entry, dict):
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_INPUT}:{input_id}")
    plan_path = _resolve_bundle_plan_path(bundle_path, raw_entry, input_id)
    actual_plan_sha256 = _validate_bundle_plan_hash(plan_path, raw_entry, input_id)

    try:
        plan = load_forced_token_plan(plan_path)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_PLAN_MISSING}:{error}") from error
    _validate_bundle_plan_metadata(plan, raw_entry, input_id)

    return ForcedTokenBundlePlan(
        input_id=input_id,
        bundle_path=str(bundle_path.resolve()),
        bundle_sha256=sha256_file(bundle_path),
        bundle_id=str(bundle.get("bundle_id")) if bundle.get("bundle_id") else None,
        plan_path=str(plan_path),
        plan_sha256=actual_plan_sha256,
        entry=raw_entry,
    )


def _resolve_bundle_plan_path(bundle_path: Path, entry: dict[str, Any], input_id: str) -> Path:
    """Resolve a bundle-relative plan while rejecting absolute and escaping paths."""

    raw_plan_path = entry.get("path")
    if not isinstance(raw_plan_path, str) or not raw_plan_path:
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_PLAN_MISSING}:{input_id}")
    relative_plan_path = Path(raw_plan_path)
    if relative_plan_path.is_absolute():
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_PLAN_METADATA}:{input_id}:plan path must be relative")
    bundle_dir = bundle_path.parent.resolve()
    plan_path = (bundle_dir / relative_plan_path).resolve()
    if plan_path != bundle_dir and bundle_dir not in plan_path.parents:
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_PLAN_METADATA}:{input_id}:plan path escapes bundle")
    if not plan_path.is_file():
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_PLAN_MISSING}:{plan_path}")
    return plan_path


def _validate_bundle_plan_hash(plan_path: Path, entry: dict[str, Any], input_id: str) -> str:
    """Validate the plan digest recorded by the immutable capture bundle."""

    actual_sha256 = sha256_file(plan_path)
    expected_sha256 = entry.get("sha256")
    if expected_sha256 != actual_sha256:
        raise ValueError(
            f"{FORCED_TOKEN_ERROR_BUNDLE_PLAN_HASH}:{input_id}:expected={expected_sha256}:actual={actual_sha256}"
        )
    return actual_sha256


def _validate_bundle_plan_metadata(plan: dict[str, Any], entry: dict[str, Any], input_id: str) -> None:
    """Require bundle metadata to match the referenced plan and input identity."""

    entry_metadata = (
        entry.get("workload_id"),
        entry.get("workload_fingerprint"),
        non_negative_int(entry.get("request_count")),
    )
    plan_metadata = (
        plan_workload_id(plan),
        plan_workload_fingerprint(plan),
        plan_request_count(plan),
    )
    if entry_metadata != plan_metadata or plan_metadata[0] != input_id:
        raise ValueError(
            f"{FORCED_TOKEN_ERROR_BUNDLE_PLAN_METADATA}:{input_id}:entry={entry_metadata}:plan={plan_metadata}"
        )


def forced_token_bundle_summary(path: Path | None) -> dict[str, Any]:
    """Return a non-throwing bundle summary for suite artifacts."""

    result: dict[str, Any] = {
        "path": str(path) if path is not None else None,
        "exists": bool(path and path.is_file()),
        "schema": None,
        "sha256": None,
        "bundle_id": None,
        "capture_suite_dir": None,
        "capture_config": None,
        "input_ids": [],
        "plan_count": 0,
        "errors": [],
        "ready": False,
    }
    if path is None or not path.is_file():
        result["errors"] = [FORCED_TOKEN_ERROR_BUNDLE_MISSING]
        return result
    result["sha256"] = sha256_file(path)
    try:
        bundle = load_forced_token_bundle(path)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        result["errors"] = [f"{FORCED_TOKEN_ERROR_BUNDLE_SCHEMA}:{error}"]
        return result
    plans = bundle["plans"]
    result.update(
        {
            "schema": bundle.get("schema"),
            "bundle_id": bundle.get("bundle_id"),
            "capture_suite_dir": bundle.get("capture_suite_dir"),
            "capture_config": bundle.get("capture_config"),
            "input_ids": sorted(str(key) for key in plans),
            "plan_count": len(plans),
            "ready": True,
        }
    )
    return result
