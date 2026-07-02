"""HiCache validation-only capacity snapshot 累加器。"""

from __future__ import annotations

import json
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from typing import Any

from ..core.facts import parse_fact_or_none


@dataclass
class HiCacheCapacityAccumulator:
    """汇总 oracle state snapshot 中的 capacity/policy 证据。"""

    snapshot_count: int = 0
    object_id_prefix_counts: Counter[str] = field(default_factory=Counter)
    unique_values: dict[str, set[str]] = field(default_factory=lambda: defaultdict(set))
    samples: list[dict[str, Any]] = field(default_factory=list)

    def observe(self, args: dict[str, Any]) -> None:
        """从 validation-only state snapshot 中汇总 capacity/policy 证据。"""

        fact = parse_fact_or_none(args)
        if fact is None or fact.fact_class != "oracle_state" or fact.role != "state_snapshot":
            return
        snapshot = args.get("state_snapshot")
        if not isinstance(snapshot, dict) or not snapshot.get("enabled", False):
            return
        capacity = snapshot.get("capacity")
        if not isinstance(capacity, dict):
            return
        self.snapshot_count += 1
        object_id = str(snapshot.get("object_id") or "unknown")
        object_id_prefix = object_id.split(":", 1)[0] if object_id else "unknown"
        self.object_id_prefix_counts[object_id_prefix] += 1
        for key, value in flatten_capacity_scalars(capacity):
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
                    "prefetch_capacity_limit_pages": capacity.get("prefetch_capacity_limit_pages"),
                }
            )

    def finalize(self) -> dict[str, Any]:
        """汇总 capacity/policy snapshot 中出现过的标量值。"""

        unique_values = {}
        for key, values in sorted(self.unique_values.items()):
            unique_values[key] = [json.loads(value) for value in sorted(values)]
        return {
            "ready": self.snapshot_count > 0,
            "snapshot_count": self.snapshot_count,
            "object_id_prefix_counts": dict(sorted(self.object_id_prefix_counts.items())),
            "unique_values": unique_values,
            "samples": self.samples,
        }


def flatten_capacity_scalars(value: Any, prefix: str = "") -> list[tuple[str, Any]]:
    """把嵌套 capacity 对象展开成可比较的标量路径。"""

    rows: list[tuple[str, Any]] = []
    if isinstance(value, dict):
        for key, item in sorted(value.items()):
            child_prefix = f"{prefix}.{key}" if prefix else str(key)
            rows.extend(flatten_capacity_scalars(item, child_prefix))
        return rows
    if value is None or isinstance(value, (list, tuple, set)):
        return rows
    if isinstance(value, (str, int, float, bool)):
        rows.append((prefix, value))
    return rows
