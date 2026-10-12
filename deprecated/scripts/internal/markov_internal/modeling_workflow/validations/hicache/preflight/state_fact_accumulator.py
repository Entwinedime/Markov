"""Streaming coverage accumulator for HiCache state-model facts."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from ..core.facts import (
    HICACHE_CONSUMER_STATE_MODEL,
    parse_fact_or_none,
)
from ..core.tokens import fact_items, token_dictionary_issue_count


STATE_FACT_REQUIRED_FIELDS_BY_ROLE = {
    "cache_lookup_input": (
        "request_id",
        "cache_scope",
        "seq_no",
        "source_page_size",
        "token_dictionary",
        "full_path_span",
        "token_count",
    ),
    "cache_lifecycle_commit": (
        "request_id",
        "cache_scope",
        "seq_no",
        "lifecycle_kind",
        "source_page_size",
        "token_dictionary",
        "full_path_span",
        "token_count",
    ),
    "cache_extend_input": (
        "cache_scope",
        "seq_no",
        "source_page_size",
        "batch_kind",
        "request_ids",
        "request_positions",
        "batch_size",
        "token_dictionaries",
        "full_path_spans",
        "token_counts",
    ),
    "prefetch_candidate_anchor": (
        "request_id",
        "cache_scope",
        "seq_no",
        "source_page_size",
        "token_dictionary",
        "full_path_span",
        "token_count",
    ),
}


@dataclass
class HiCacheStateFactAccumulator:
    """Check state facts after the shared schema has validated their routing."""

    missing_required_fact_events: int = 0
    invalid_token_dictionary_issue_count: int = 0
    dictionary_ids: set[str] = field(default_factory=set)
    dictionary_ids_with_tokens: set[str] = field(default_factory=set)
    span_path_ids: set[str] = field(default_factory=set)
    last_seq_by_scope: dict[str, int] = field(default_factory=dict)
    seq_order_error_count: int = 0

    def observe(self, args: dict[str, Any]) -> None:
        """Consume one event, retaining only the previous sequence per scope."""

        fact = parse_fact_or_none(args)
        if fact is None or not fact.has_consumer(HICACHE_CONSUMER_STATE_MODEL):
            return

        # Routing validation admits exactly the workload-identity roles below.
        # Extend records are inputs at start; all other facts commit at end.
        expected_phase = "start" if fact.role == "cache_extend_input" else "end"
        if str(args.get("phase") or "").lower() != expected_phase:
            return

        dictionary_field, span_field = (
            ("token_dictionaries", "full_path_spans")
            if fact.role == "cache_extend_input"
            else ("token_dictionary", "full_path_span")
        )
        dictionaries = fact_items(args.get(dictionary_field))
        spans = fact_items(args.get(span_field))
        missing = any(not _present(args.get(name)) for name in STATE_FACT_REQUIRED_FIELDS_BY_ROLE[fact.role])
        for items, identity in ((dictionaries, "token_path_id"), (spans, "path_id")):
            missing |= not items or any(not _token_reference(item, identity) for item in items)
        self.missing_required_fact_events += missing

        for item in dictionaries:
            if isinstance(item, dict):
                self._observe_dictionary(item)
        for item in spans:
            if isinstance(item, dict):
                path_id = item.get("path_id")
                if isinstance(path_id, str) and path_id:
                    self.span_path_ids.add(path_id)

        scope = args.get("cache_scope")
        seq_no = _int_or_none(args.get("seq_no"))
        if _present(scope) and seq_no is not None:
            scope = str(scope)
            previous = self.last_seq_by_scope.get(scope)
            self.seq_order_error_count += previous is not None and seq_no <= previous
            self.last_seq_by_scope[scope] = seq_no

    def finalize(self) -> dict[str, Any]:
        """Summarize missing fields, token references and sequence violations."""

        coverage = {
            "missing_required_fact_events": self.missing_required_fact_events,
            "missing_token_dictionary_refs": sorted(self.span_path_ids - self.dictionary_ids),
            "dictionary_ids_without_tokens": sorted(self.dictionary_ids - self.dictionary_ids_with_tokens),
            "invalid_token_dictionary_issue_count": self.invalid_token_dictionary_issue_count,
            "seq_order_error_count": self.seq_order_error_count,
        }
        return {**coverage, "ready": not any(coverage.values())}

    def _observe_dictionary(self, item: dict[str, Any]) -> None:
        token_path_id = item.get("token_path_id")
        if not isinstance(token_path_id, str) or not token_path_id:
            return
        self.dictionary_ids.add(token_path_id)
        if isinstance(item.get("token_ids"), list):
            self.dictionary_ids_with_tokens.add(token_path_id)
            self.invalid_token_dictionary_issue_count += token_dictionary_issue_count(item)


def _present(value: Any) -> bool:
    if value is None or isinstance(value, bool):
        return False
    return bool(value) if isinstance(value, (str, list, tuple, set, dict)) else True


def _token_reference(value: Any, identity: str) -> bool:
    return (
        isinstance(value, dict)
        and isinstance(value.get(identity), str)
        and bool(value[identity])
        and all(_present(value.get(field)) for field in ("token_count", "hash_algo"))
    )


def _int_or_none(value: Any) -> int | None:
    if value is None or isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None
