"""HiCache fact 解析与 consumer 路由 helper。"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


from ..common.paths import ROOT_DIR

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
    """从 Chrome trace event args 中解析出的 fact 合同。"""

    fact_class: str
    role: str
    consumers: tuple[str, ...]

    def has_consumer(self, consumer: str) -> bool:
        """判断该 fact 是否声明给指定 consumer。"""

        return consumer in self.consumers


def parse_fact(args: dict[str, Any]) -> HiCacheFact:
    """解析并校验 event args 中必须存在的 HiCache fact 对象。"""

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
    """解析携带 fact 的 event；非 HiCache event 不携带 fact 时返回 None。"""

    if "fact" not in args:
        if _is_hicache_probe_args(args):
            raise ValueError("HiCache trace event args must contain fact object")
        return None
    return parse_fact(args)


def _is_hicache_probe_args(args: dict[str, Any]) -> bool:
    """判断 args 是否看起来来自 HiCache Python probe。"""

    target_id = str(args.get("target_id") or "").lower()
    return target_id.startswith(("hiradix.", "hicache.", "hicache_controller."))


def fact_has_consumer(args: dict[str, Any], consumer: str) -> bool:
    """判断 event args 是否声明了指定 consumer。"""

    fact = parse_fact_or_none(args)
    return fact is not None and fact.has_consumer(consumer)


def fact_key(args: dict[str, Any]) -> tuple[str, str] | None:
    """返回 fact event 的 `(class, role)` 路由键。"""

    fact = parse_fact_or_none(args)
    if fact is None:
        return None
    return (fact.fact_class, fact.role)
