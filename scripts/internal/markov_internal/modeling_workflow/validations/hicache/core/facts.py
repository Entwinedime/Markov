"""Strict parsing of HiCache fact metadata emitted by Python probes."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from markov_internal.common.paths import prepend_repo_src_to_sys_path

prepend_repo_src_to_sys_path()

from profiling.python_probe.trace_sim_probe.schema import (  # noqa: E402
    HICACHE_CONSUMER_DAG_PATCH,
    HICACHE_CONSUMER_STATE_MODEL,
    validate_hicache_fact,
)


__all__ = [
    "HICACHE_CONSUMER_DAG_PATCH",
    "HICACHE_CONSUMER_STATE_MODEL",
    "HiCacheFact",
    "parse_fact_or_none",
]


@dataclass(frozen=True)
class HiCacheFact:
    """Validated routing metadata parsed from Chrome trace event arguments."""

    fact_class: str
    role: str
    consumers: tuple[str, ...]

    def has_consumer(self, consumer: str) -> bool:
        """Return whether the probe declared this consumer for the fact."""

        return consumer in self.consumers


def parse_fact_or_none(args: dict[str, Any]) -> HiCacheFact | None:
    """Validate declared facts; unrelated events return None.

    Known HiCache probe targets must carry fact metadata. They cannot be
    mistaken for unrelated trace rows when that metadata is missing.
    """

    if "fact" not in args:
        target_id = str(args.get("target_id") or "").lower()
        if target_id.startswith(("hiradix.", "hicache.", "hicache_controller.")):
            raise ValueError("HiCache trace event args must contain fact object")
        return None

    fact = args.get("fact")
    if not isinstance(fact, dict):
        raise ValueError("trace event args must contain fact object")
    fact_class = fact.get("class")
    role = fact.get("role")
    consumers = fact.get("consumers")
    if not isinstance(fact_class, str) or not fact_class:
        raise ValueError("trace event fact.class must be a non-empty string")
    if not isinstance(role, str) or not role:
        raise ValueError("trace event fact.role must be a non-empty string")
    if not isinstance(consumers, list) or not all(isinstance(item, str) and item for item in consumers):
        raise ValueError("trace event fact.consumers must be a non-empty string array")
    validate_hicache_fact(fact_class, role, consumers)
    return HiCacheFact(fact_class=fact_class, role=role, consumers=tuple(consumers))
