"""Select and validate Python probe targets from the active catalog."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.paths import ROOT_DIR, prepend_repo_src_to_sys_path

prepend_repo_src_to_sys_path()

from profiling.config import PYTHON_PROBE_DIAGNOSTICS  # noqa: E402
from profiling.python_probe.trace_sim_probe.schema import validate_hicache_fact  # noqa: E402


DEFAULT_HICACHE_TARGET_CATALOG = ROOT_DIR / "configs/profiling/hicache_probe_targets.json"
FACT_CONSUMERS = {
    "workload_identity": ("hicache_state_model", "hicache_input_contract"),
    "source_actual": ("hicache_dag_patch",),
    "timing_observation": ("hicache_dag_patch",),
}


def select_python_probe_targets(
    requested_consumers: tuple[str, ...],
    *,
    diagnostics: str = "off",
) -> list[dict[str, Any]]:
    """Select this run's capture contract by requested consumer.

    Each target's ``events`` mapping declares whether it emits start or end
    records. The exact selected objects are shared by server injection,
    the profile manifest, and post-capture quality audit.
    """

    if diagnostics not in PYTHON_PROBE_DIAGNOSTICS:
        allowed = ", ".join(sorted(PYTHON_PROBE_DIAGNOSTICS))
        raise ValueError(f"python probe diagnostics must be one of: {allowed}")
    catalog_path = DEFAULT_HICACHE_TARGET_CATALOG
    raw_targets = load_json(catalog_path)
    if not isinstance(raw_targets, list):
        raise ValueError(f"python probe target catalog must be a JSON array: {catalog_path}")

    selected: list[dict[str, Any]] = []
    for index, raw_target in enumerate(raw_targets):
        target = validated_catalog_target(raw_target, index, catalog_path)
        _resolve_thread_timing_policy(target, diagnostics)
        if target.get("diagnostics", "off") == "full" and diagnostics != "full":
            continue
        fact = target["fact"]
        allowed = set(FACT_CONSUMERS[fact["class"]])
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
    """Validate and copy one catalog entry before consumer filtering."""

    if not isinstance(raw, dict):
        raise ValueError(f"{catalog_path}: targets[{index}] must be an object")
    target = dict(raw)
    prefix = f"{catalog_path}: targets[{index}]"
    _validate_target_identity(target, prefix)
    _validate_target_events(target.get("events"), prefix)
    _validate_target_fact(target.get("fact"), prefix)
    _validate_target_diagnostics(target.get("diagnostics", "off"), prefix)
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
    allowed_phases = {"start", "end"}
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
    if not isinstance(fact_class, str) or not fact_class:
        raise ValueError(f"{prefix}.fact.class must be a non-empty string")
    if not isinstance(role, str) or not role:
        raise ValueError(f"{prefix}.fact.role must be a non-empty string")
    consumers = FACT_CONSUMERS.get(fact_class)
    if consumers is None:
        raise ValueError(f"{prefix}.fact.class is unsupported: {fact_class!r}")
    validate_hicache_fact(fact_class, role, consumers)


def _validate_target_diagnostics(value: Any, prefix: str) -> None:
    """Validate the catalog-owned capture policy without inferring it from identity."""

    if not isinstance(value, str) or value not in PYTHON_PROBE_DIAGNOSTICS:
        allowed = ", ".join(sorted(PYTHON_PROBE_DIAGNOSTICS))
        raise ValueError(f"{prefix}.diagnostics must be one of: {allowed}")


def _resolve_thread_timing_policy(target: dict[str, Any], diagnostics: str) -> None:
    """Materialize a catalog-declared thread-timing policy for the probe parser."""

    value = target.get("capture_thread_timing")
    if value is None or isinstance(value, bool):
        return
    if not isinstance(value, dict) or set(value) != {"diagnostics"}:
        raise ValueError(f"python probe target {target['id']!r} capture_thread_timing policy is invalid")
    required = value["diagnostics"]
    if required not in PYTHON_PROBE_DIAGNOSTICS or required == "off":
        raise ValueError(f"python probe target {target['id']!r} thread timing requires a diagnostic mode")
    if diagnostics == required:
        target["capture_thread_timing"] = True
    else:
        target.pop("capture_thread_timing")
