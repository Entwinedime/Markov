"""Streaming coverage accumulator for HiCache state-model facts."""

from __future__ import annotations

from collections import Counter, defaultdict
from dataclasses import dataclass, field
from typing import Any

from ..core.facts import (
    HICACHE_CONSUMER_STATE_MODEL,
    HiCacheFact,
    parse_fact_or_none,
)
from ..core.tokens import fact_items, token_dictionary_issues
from .mechanisms import completed_fact_role, is_hicache_profile_event


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

STATE_FACT_DICTIONARY_FIELDS_BY_ROLE = {
    "cache_lookup_input": ("token_dictionary",),
    "cache_lifecycle_commit": ("token_dictionary",),
    "cache_extend_input": ("token_dictionaries",),
    "prefetch_candidate_anchor": ("token_dictionary",),
}

STATE_FACT_SPAN_FIELDS_BY_ROLE = {
    "cache_lookup_input": ("full_path_span",),
    "cache_lifecycle_commit": ("full_path_span",),
    "cache_extend_input": ("full_path_spans",),
    "prefetch_candidate_anchor": ("full_path_span",),
}


@dataclass
class HiCacheStateFactAccumulator:
    """Accumulate field, routing, token, and sequence contract evidence."""

    counts: Counter[str] = field(default_factory=Counter)
    invalid_token_dictionary_issue_count: int = 0
    dictionary_ids: set[str] = field(default_factory=set)
    dictionary_ids_with_tokens: set[str] = field(default_factory=set)
    span_path_ids: set[str] = field(default_factory=set)
    seq_by_scope: dict[str, list[int]] = field(default_factory=lambda: defaultdict(list))

    def observe(self, args: dict[str, Any]) -> None:
        """Observe one event without retaining the complete trace payload."""

        if not is_hicache_profile_event(args, set(STATE_FACT_REQUIRED_FIELDS_BY_ROLE)):
            return
        fact = parse_fact_or_none(args)
        if fact is None:
            return
        if self._is_completed_state_model_fact(args, fact):
            self._observe_token_references(args, fact.role)
        if not fact.has_consumer(HICACHE_CONSUMER_STATE_MODEL):
            return
        if fact.fact_class != "workload_identity":
            self.counts["state_model_consumer_on_non_state_fact"] += 1
            return
        self.counts["state_model_events"] += 1
        if fact.role not in STATE_FACT_REQUIRED_FIELDS_BY_ROLE:
            self._observe_unknown_role(args, fact.role)
            return
        if not completed_fact_role(args, fact.role):
            return
        self._observe_completed_required_fact(args, fact.role)

    def finalize(self) -> dict[str, Any]:
        """Return an immutable-style summary of all accumulated evidence."""

        missing_token_dictionary_refs = sorted(self.span_path_ids - self.dictionary_ids)
        dictionary_ids_without_tokens = sorted(self.dictionary_ids - self.dictionary_ids_with_tokens)
        route_error_events = (
            self.counts["state_model_consumer_on_non_state_fact"] + self.counts["unknown_state_model_role_events"]
        )
        seq_order_error_count = self._seq_order_error_count()
        return {
            "missing_required_fact_events": self.counts["missing_required_fact_events"],
            "route_error_events": route_error_events,
            "missing_token_dictionary_refs": missing_token_dictionary_refs,
            "dictionary_ids_without_tokens": dictionary_ids_without_tokens,
            "invalid_token_dictionary_issue_count": self.invalid_token_dictionary_issue_count,
            "seq_order_error_count": seq_order_error_count,
            "ready": self._ready(
                route_error_events,
                missing_token_dictionary_refs,
                dictionary_ids_without_tokens,
                self.invalid_token_dictionary_issue_count,
                seq_order_error_count,
            ),
        }

    def _observe_unknown_role(self, args: dict[str, Any], role: str) -> None:
        self.counts["unknown_state_model_role_events"] += 1
        if completed_fact_role(args, role):
            self.counts["missing_required_fact_events"] += 1

    def _observe_completed_required_fact(self, args: dict[str, Any], role: str) -> None:
        missing = self._missing_fields(args, role)
        if missing:
            self.counts["missing_required_fact_events"] += 1

        scope = args.get("cache_scope")
        seq_no = _int_or_none(args.get("seq_no"))
        if _present(scope) and seq_no is not None:
            self.seq_by_scope[str(scope)].append(seq_no)

    def _observe_token_references(self, args: dict[str, Any], role: str) -> None:
        for field_name in STATE_FACT_DICTIONARY_FIELDS_BY_ROLE.get(role, ()):
            for item in fact_items(args.get(field_name)):
                if isinstance(item, dict):
                    self._observe_dictionary(item)
        for field_name in STATE_FACT_SPAN_FIELDS_BY_ROLE.get(role, ()):
            for item in fact_items(args.get(field_name)):
                if isinstance(item, dict):
                    path_id = item.get("path_id")
                    if isinstance(path_id, str) and path_id:
                        self.span_path_ids.add(path_id)

    def _observe_dictionary(self, item: dict[str, Any]) -> None:
        token_path_id = item.get("token_path_id")
        if not isinstance(token_path_id, str) or not token_path_id:
            return
        self.dictionary_ids.add(token_path_id)
        if isinstance(item.get("token_ids"), list):
            self.dictionary_ids_with_tokens.add(token_path_id)
            self.invalid_token_dictionary_issue_count += len(token_dictionary_issues(item))

    def _missing_fields(self, args: dict[str, Any], role: str) -> list[str]:
        missing = [field for field in STATE_FACT_REQUIRED_FIELDS_BY_ROLE.get(role, ()) if not _present(args.get(field))]
        for field_name in STATE_FACT_DICTIONARY_FIELDS_BY_ROLE.get(role, ()):
            items = fact_items(args.get(field_name))
            if args.get(field_name) is not None and (
                not items or any(not _token_reference(item, "token_path_id") for item in items)
            ):
                missing.append(f"{field_name}.token_path_id")
        for field_name in STATE_FACT_SPAN_FIELDS_BY_ROLE.get(role, ()):
            items = fact_items(args.get(field_name))
            if args.get(field_name) is not None and (not items or any(not _token_reference(item, "path_id") for item in items)):
                missing.append(f"{field_name}.path_id")
        return missing

    def _is_completed_state_model_fact(self, args: dict[str, Any], fact: HiCacheFact) -> bool:
        return bool(
            completed_fact_role(args, fact.role)
            and fact.has_consumer(HICACHE_CONSUMER_STATE_MODEL)
            and fact.fact_class == "workload_identity"
        )

    def _seq_order_error_count(self) -> int:
        error_count = 0
        for seq_values in self.seq_by_scope.values():
            previous = None
            for value in seq_values:
                if previous is not None and value <= previous:
                    error_count += 1
                previous = value
        return error_count

    def _ready(
        self,
        route_error_events: int,
        missing_token_dictionary_refs: list[str],
        dictionary_ids_without_tokens: list[str],
        invalid_token_dictionary_issue_count: int,
        seq_order_error_count: int,
    ) -> bool:
        return (
            self.counts["missing_required_fact_events"] == 0
            and route_error_events == 0
            and not missing_token_dictionary_refs
            and not dictionary_ids_without_tokens
            and invalid_token_dictionary_issue_count == 0
            and seq_order_error_count == 0
        )


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
