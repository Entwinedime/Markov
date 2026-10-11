"""Parse independent control measurement inputs for explicit database import."""

from __future__ import annotations

from typing import Any

from ...common.paths import require_repo_path, repo_relative_path


_SHARED_OPERATIONS = (
    "release_host",
    "write_confirmation",
    "prefetch_query",
    "load_index",
    "load_submission",
    "layer_wait",
)
CALIBRATION_FIELDS = tuple(f"{kind}_calibration" for kind in ("prefetch_wait", "write_host", *_SHARED_OPERATIONS)) + (
    "prefetch_cpu_calibrations",
)


def control_calibrations(*collections: dict[str, Any]) -> dict[str, Any]:
    """Parse and merge declarations; later entries override the same operation.

    Paths are repository-relative. Inputs are not mutated; operation semantics
    remain the C++ consumer's responsibility.
    """

    def path(value: str) -> str:
        if not isinstance(value, str) or not value:
            raise ValueError("control calibration requires a nonempty file path")
        return str(repo_relative_path(require_repo_path(value)))

    result: dict[str, Any] = {"prefetch_wait": {}, "write_host": {}}
    for raw in collections:
        if not isinstance(raw, dict) or set(raw) - {"prefetch_wait", "write_host", *_SHARED_OPERATIONS}:
            raise ValueError("invalid shared control_calibrations fields")
        for policy, value in raw.get("prefetch_wait", {}).items():
            if policy not in {"best_effort", "timeout", "wait_complete"}:
                raise ValueError(f"unknown calibration prefetch policy: {policy}")
            result["prefetch_wait"][policy] = path(value)
        for page, policies in raw.get("write_host", {}).items():
            if not isinstance(page, str) or not page.isdecimal() or int(page) <= 0 or str(int(page)) != page:
                raise ValueError("write calibration page size must be a positive decimal key")
            writes = result["write_host"].setdefault(page, {})
            for policy, value in policies.items():
                if policy not in {"write_back", "write_through", "write_through_selective"}:
                    raise ValueError(f"unknown calibration write policy: {policy}")
                writes[policy] = path(value)
        for kind in _SHARED_OPERATIONS:
            if kind in raw:
                result[kind] = path(raw[kind])
    return result
