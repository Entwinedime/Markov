"""Collection and parsing of values from HiCache capacity snapshots."""

from __future__ import annotations

import json
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from typing import Any


@dataclass
class HiCacheCapacityEvidence:
    """Accumulate validation-only capacity and policy snapshot evidence."""

    snapshot_count: int = 0
    object_id_prefix_counts: Counter[str] = field(default_factory=Counter)
    unique_values: dict[str, set[str]] = field(default_factory=lambda: defaultdict(set))
    samples: list[dict[str, Any]] = field(default_factory=list)

    def observe_snapshot(self, snapshot: dict[str, Any]) -> None:
        """Consume one enabled state snapshot when it carries capacity data."""

        if not snapshot.get("enabled", False):
            return
        capacity = snapshot.get("capacity")
        if not isinstance(capacity, dict):
            return
        self.snapshot_count += 1
        object_id = str(snapshot.get("object_id") or "unknown")
        object_id_prefix = object_id.split(":", 1)[0] if object_id else "unknown"
        self.object_id_prefix_counts[object_id_prefix] += 1
        for key, value in flatten_hicache_capacity_scalars(capacity):
            self.unique_values[key].add(json.dumps(value, ensure_ascii=False, sort_keys=True))
        if len(self.samples) < 5:
            self.samples.append(
                {
                    "object_id_prefix": object_id_prefix,
                    "page_size": capacity.get("page_size"),
                    "write_policy": capacity.get("write_policy"),
                    "prefetch_policy": capacity.get("prefetch_policy"),
                    "l1_capacity_pages": capacity.get("l1_capacity_pages"),
                    "l1_available_pages": capacity.get("l1_available_pages"),
                    "l2_capacity_pages": capacity.get("l2_capacity_pages"),
                    "l2_available_pages": capacity.get("l2_available_pages"),
                    "prefetch_threshold_pages": capacity.get("prefetch_threshold_pages"),
                    "prefetch_capacity_limit_pages": capacity.get("prefetch_capacity_limit_pages"),
                }
            )

    def as_payload(self) -> dict[str, Any]:
        """Return the deterministic JSON representation used by audits."""

        return {
            "ready": self.snapshot_count > 0,
            "snapshot_count": self.snapshot_count,
            "object_id_prefix_counts": dict(sorted(self.object_id_prefix_counts.items())),
            "unique_values": {
                key: [json.loads(value) for value in sorted(values)]
                for key, values in sorted(self.unique_values.items())
            },
            "samples": self.samples,
        }


def unique_int_values(unique_values: dict[str, Any], keys: list[str]) -> list[int]:
    """Collect unique integer values from selected capacity paths."""

    values: set[int] = set()
    for key in keys:
        raw_values = unique_values.get(key)
        if not isinstance(raw_values, list):
            continue
        for value in raw_values:
            item = parse_int_or_none(value)
            if item is not None:
                values.add(item)
    return sorted(values)


def unique_policy_values(unique_values: dict[str, Any], keys: list[str]) -> list[str]:
    """Collect normalized policy values from selected capacity paths."""

    values: set[str] = set()
    for key in keys:
        raw_values = unique_values.get(key)
        if not isinstance(raw_values, list):
            continue
        for value in raw_values:
            normalized = normalize_policy_value(value)
            if normalized:
                values.add(normalized)
    return sorted(values)


def parse_int_or_none(value: Any) -> int | None:
    """Parse an integer candidate while rejecting booleans and invalid values."""

    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def normalize_policy_value(value: Any) -> str:
    """Normalize a policy string to lowercase underscore form."""

    if value is None:
        return ""
    return str(value).strip().lower().replace("-", "_")


def flatten_hicache_capacity_scalars(value: Any, prefix: str = "") -> list[tuple[str, Any]]:
    """Flatten nested capacity scalars to dotted paths."""

    rows: list[tuple[str, Any]] = []
    if isinstance(value, dict):
        for key, item in sorted(value.items()):
            child_prefix = f"{prefix}.{key}" if prefix else str(key)
            rows.extend(flatten_hicache_capacity_scalars(item, child_prefix))
        return rows
    if value is None or isinstance(value, (list, tuple, set)):
        return rows
    if isinstance(value, (str, int, float, bool)):
        rows.append((prefix, value))
    return rows
