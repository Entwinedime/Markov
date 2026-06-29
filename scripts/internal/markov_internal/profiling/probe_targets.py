"""Python probe target catalog 选择工具。"""

from __future__ import annotations

import copy
import sys
from pathlib import Path
from typing import Any

from ..common.io import load_json
from ..common.paths import ROOT_DIR, resolve_repo_path

sys.path.insert(0, str(ROOT_DIR / "src"))

from profiling.python_probe.trace_sim_probe.schema import validate_hicache_fact  # noqa: E402


DEFAULT_HICACHE_TARGET_CATALOG = ROOT_DIR / "configs/profiling/hicache_probe_targets.json"


def select_python_probe_targets(
    catalog_value: str | None,
    requested_consumers: tuple[str, ...],
) -> list[dict[str, Any]]:
    """按 requested consumers 从 catalog 选择本次 run 的采集合同。

    target 是否发 start/end/instant 由 catalog 的 `events` phase key 声明；选择结果同时用于
    server 注入、manifest 和 quality 审计。
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
    """校验并复制一个 target catalog entry。"""

    if not isinstance(raw, dict):
        raise ValueError(f"{catalog_path}: targets[{index}] must be an object")
    target = copy.deepcopy(raw)
    prefix = f"{catalog_path}: targets[{index}]"
    for key in ("id", "module", "target"):
        if not isinstance(target.get(key), str) or not target.get(key):
            raise ValueError(f"{prefix}.{key} must be a non-empty string")
    if "phases" in target or "emit_phases" in target:
        raise ValueError(f"{prefix} must declare phases only as keys of events")
    events = target.get("events")
    if not isinstance(events, dict) or not events:
        raise ValueError(f"{prefix}.events must be a non-empty phase-to-event-name object")
    allowed_phases = {"start", "end", "exception", "instant"}
    for phase, event_name in events.items():
        if phase not in allowed_phases:
            raise ValueError(f"{prefix}.events contains unsupported phase {phase!r}")
        if not isinstance(event_name, str) or not event_name:
            raise ValueError(f"{prefix}.events[{phase!r}] must be a non-empty string")
    fact = target.get("fact")
    if not isinstance(fact, dict):
        raise ValueError(f"{prefix}.fact must be an object")
    if set(fact) != {"class", "role", "consumers"}:
        raise ValueError(f"{prefix}.fact must contain only class, role, and consumers")
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
    return target
