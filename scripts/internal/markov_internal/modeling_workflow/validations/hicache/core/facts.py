"""Strict parsing of HiCache fact metadata emitted by Python probes."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from markov_internal.common.paths import prepend_repo_src_to_sys_path

prepend_repo_src_to_sys_path()

from profiling.python_probe.trace_sim_probe.schema import (  # noqa: E402
    HICACHE_CONSUMER_INPUT_CONTRACT,
    HICACHE_CONSUMER_DAG_PATCH,
    HICACHE_CONSUMER_STATE_MODEL,
    validate_hicache_fact,
)


__all__ = [
    "HICACHE_CONSUMER_INPUT_CONTRACT",
    "HICACHE_CONSUMER_DAG_PATCH",
    "HICACHE_CONSUMER_STATE_MODEL",
    "HiCacheFact",
    "parse_fact",
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


def parse_fact(args: dict[str, Any]) -> HiCacheFact:
    """Parse and validate the required HiCache ``fact`` object."""

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


def parse_fact_or_none(args: dict[str, Any]) -> HiCacheFact | None:
    """Parse a fact, or return ``None`` for an unrelated trace event.

    Events carrying a known HiCache probe target remain strict: omitting their
    fact metadata is a capture-contract violation rather than an unrelated row.
    """

    if "fact" not in args:
        if _is_hicache_probe_args(args):
            raise ValueError("HiCache trace event args must contain fact object")
        return None
    return parse_fact(args)


def _is_hicache_probe_args(args: dict[str, Any]) -> bool:
    """Return whether arguments identify a known HiCache probe namespace."""

    target_id = str(args.get("target_id") or "").lower()
    return target_id.startswith(("hiradix.", "hicache.", "hicache_controller."))
