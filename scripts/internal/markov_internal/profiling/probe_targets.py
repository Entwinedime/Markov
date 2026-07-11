"""Select and validate Python probe targets from the active catalog."""

from __future__ import annotations

import copy
from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.paths import ROOT_DIR, prepend_repo_src_to_sys_path, resolve_repo_path

prepend_repo_src_to_sys_path()

from profiling.python_probe.trace_sim_probe.schema import validate_hicache_fact  # noqa: E402


DEFAULT_HICACHE_TARGET_CATALOG = ROOT_DIR / "configs/profiling/hicache_probe_targets.json"


def select_python_probe_targets(
    catalog_value: str | None,
    requested_consumers: tuple[str, ...],
) -> list[dict[str, Any]]:
    """Select this run's capture contract by requested consumer.

    Each target's ``events`` mapping declares whether it emits start, end, or
    instant records. The exact selected objects are shared by server injection,
    the profile manifest, and post-capture quality audit.
    """

    catalog_path = resolve_repo_path(catalog_value) or DEFAULT_HICACHE_TARGET_CATALOG
    raw_targets = load_json(catalog_path)
    if not isinstance(raw_targets, list):
        raise ValueError(f"python probe target catalog must be a JSON array: {catalog_path}")

    selected: list[dict[str, Any]] = []
    for index, raw_target in enumerate(raw_targets):
        target = validated_catalog_target(raw_target, index, catalog_path)
        fact = target["fact"]
        allowed = set(fact["consumers"])
        selected_consumers = [consumer for consumer in requested_consumers if consumer in allowed]
        if not selected_consumers:
            continue
        target["fact"] = {
            "class": fact["class"],
            "role": fact["role"],
            "consumers": selected_consumers,
        }
        selected.append(target)
    return selected


def validated_catalog_target(raw: Any, index: int, catalog_path: Path) -> dict[str, Any]:
    """Validate and deep-copy one catalog entry before consumer filtering."""

    if not isinstance(raw, dict):
        raise ValueError(f"{catalog_path}: targets[{index}] must be an object")
    target = copy.deepcopy(raw)
    prefix = f"{catalog_path}: targets[{index}]"
    _validate_target_identity(target, prefix)
    _validate_target_events(target.get("events"), prefix)
    _validate_target_fact(target.get("fact"), prefix)
    return target


def _validate_target_identity(target: dict[str, Any], prefix: str) -> None:
    """Require stable catalog identity and import-target fields."""

    for key in ("id", "module", "target"):
        if not isinstance(target.get(key), str) or not target.get(key):
            raise ValueError(f"{prefix}.{key} must be a non-empty string")


def _validate_target_events(events: Any, prefix: str) -> None:
    """Validate the phase-to-event mapping emitted by one probe target."""

    if not isinstance(events, dict) or not events:
        raise ValueError(f"{prefix}.events must be a non-empty phase-to-event-name object")
    allowed_phases = {"start", "end", "exception", "instant"}
    for phase, event_name in events.items():
        if phase not in allowed_phases:
            raise ValueError(f"{prefix}.events contains unsupported phase {phase!r}")
        if not isinstance(event_name, str) or not event_name:
            raise ValueError(f"{prefix}.events[{phase!r}] must be a non-empty string")


def _validate_target_fact(fact: Any, prefix: str) -> None:
    """Validate one target's routed HiCache fact declaration."""

    if not isinstance(fact, dict):
        raise ValueError(f"{prefix}.fact must be an object")
    fact_class = fact.get("class")
    role = fact.get("role")
    consumers = fact.get("consumers")
    if not isinstance(fact_class, str) or not fact_class:
        raise ValueError(f"{prefix}.fact.class must be a non-empty string")
    if not isinstance(role, str) or not role:
        raise ValueError(f"{prefix}.fact.role must be a non-empty string")
    if not isinstance(consumers, list) or not all(isinstance(item, str) and item for item in consumers):
        raise ValueError(f"{prefix}.fact.consumers must be a non-empty string array")
    validate_hicache_fact(fact_class, role, consumers)
