"""HiCache fact parsing helpers for profiling/modeling scripts."""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROOT_DIR = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from profiling.python_probe.trace_sim_probe.schema import (  # noqa: E402
    HICACHE_CONSUMER_FINAL_STATE_VALIDATOR,
    HICACHE_CONSUMER_INPUT_CONTRACT,
    HICACHE_CONSUMER_STATE_MODEL,
    HICACHE_CONSUMER_TRANSITION_VALIDATOR,
    validate_hicache_fact,
)


@dataclass(frozen=True)
class HiCacheFact:
    """Parsed fact contract from a Chrome trace event args object."""

    fact_class: str
    role: str
    consumers: tuple[str, ...]

    def has_consumer(self, consumer: str) -> bool:
        """Return whether this event is declared for a consumer."""

        return consumer in self.consumers


def parse_fact(args: dict[str, Any]) -> HiCacheFact:
    """Parse and validate the required HiCache fact object from event args."""

    fact = args.get("fact")
    if not isinstance(fact, dict):
        raise ValueError("trace event args must contain fact object")
    if set(fact) != {"class", "role", "consumers"}:
        raise ValueError("trace event fact must contain only class, role, and consumers")
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
    """Return a parsed fact for events that carry one, otherwise None."""

    if "fact" not in args:
        if _is_hicache_probe_args(args):
            raise ValueError("HiCache trace event args must contain fact object")
        return None
    return parse_fact(args)


def _is_hicache_probe_args(args: dict[str, Any]) -> bool:
    """Return whether args look like a HiCache Python probe event."""

    target_id = str(args.get("target_id") or "").lower()
    return target_id.startswith(("hiradix.", "hicache.", "hicache_controller."))


def fact_has_consumer(args: dict[str, Any], consumer: str) -> bool:
    """Return whether event args declare a fact for a consumer."""

    fact = parse_fact_or_none(args)
    return fact is not None and fact.has_consumer(consumer)


def fact_key(args: dict[str, Any]) -> tuple[str, str] | None:
    """Return `(class, role)` for fact-bearing events."""

    fact = parse_fact_or_none(args)
    if fact is None:
        return None
    return (fact.fact_class, fact.role)
