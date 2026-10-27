"""Select and validate Python probe targets from the active catalog."""

from __future__ import annotations

from typing import Any

from ..common.io import load_json
from ..common.paths import ROOT_DIR, prepend_repo_src_to_sys_path

prepend_repo_src_to_sys_path()

from profiling.config import PYTHON_PROBE_DIAGNOSTICS  # noqa: E402
from profiling.python_probe.trace_sim_probe.schema import parse_target  # noqa: E402


DEFAULT_HICACHE_TARGET_CATALOG = ROOT_DIR / "configs/profiling/hicache_probe_targets.json"


def select_python_probe_targets(
    requested_consumers: tuple[str, ...],
    *,
    diagnostics: str = "off",
) -> list[dict[str, Any]]:
    """Select this run's capture contract by requested consumer.

    Each target's ``events`` mapping declares whether it emits start or end
    records. The exact selected objects are shared by server injection,
    the profile manifest, and post-capture quality audit.
    Consumer and diagnostic choices come from the normalized runtime config;
    this boundary validates the catalog, not those choices again.
    """

    catalog_path = DEFAULT_HICACHE_TARGET_CATALOG
    raw_targets = load_json(catalog_path)
    if not isinstance(raw_targets, list):
        raise ValueError(f"python probe target catalog must be a JSON array: {catalog_path}")

    selected: list[dict[str, Any]] = []
    for index, raw_target in enumerate(raw_targets):
        if not isinstance(raw_target, dict):
            raise ValueError(f"{catalog_path}: targets[{index}] must be an object")
        target = dict(raw_target)
        for key in ("capture_thread_timing", "capture_emission_timing"):
            _resolve_capture_policy(target, key, diagnostics)

        specification = parse_target(target, requested_consumers=requested_consumers)
        fact = specification.fact
        if not fact.consumers:
            continue
        target["fact"] = {
            "class": fact.fact_class,
            "role": fact.role,
            "consumers": list(fact.consumers),
        }
        selected.append(target)
    return selected


def _resolve_capture_policy(target: dict[str, Any], key: str, diagnostics: str) -> None:
    """Resolve optional thread and emission clocks from the catalog."""

    value = target.get(key)
    if value is None or isinstance(value, bool):
        return
    if not isinstance(value, dict) or set(value) != {"diagnostics"}:
        raise ValueError(f"python probe target {target['id']!r} {key} policy is invalid")
    required = value["diagnostics"]
    if required not in PYTHON_PROBE_DIAGNOSTICS or required == "off":
        raise ValueError(f"python probe target {target['id']!r} {key} requires a diagnostic mode")
    if diagnostics == required:
        target[key] = True
    else:
        target.pop(key)
