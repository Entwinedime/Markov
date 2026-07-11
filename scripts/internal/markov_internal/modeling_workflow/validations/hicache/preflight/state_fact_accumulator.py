"""Streaming coverage accumulator for HiCache state-model facts."""

from __future__ import annotations

from collections import Counter, defaultdict
from dataclasses import dataclass, field
from typing import Any

from ..core.facts import (
    HICACHE_CONSUMER_INPUT_CONTRACT,
    HICACHE_CONSUMER_STATE_MODEL,
    HiCacheFact,
    parse_fact_or_none,
)
from ..core.tokens import fact_items, token_dictionary_issues
from .mechanisms import completed_fact_role, is_hicache_profile_event
from .state_fact_checks import (
    batch_state_fact_errors,
    has_fact,
    has_token_dictionary,
    has_token_span,
    int_or_none,
)


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
    class_events: Counter[str] = field(default_factory=Counter)
    role_events: Counter[str] = field(default_factory=Counter)
    consumer_events: Counter[str] = field(default_factory=Counter)
    role_completed_events: Counter[str] = field(default_factory=Counter)
    missing_fields: Counter[str] = field(default_factory=Counter)
    missing_fields_by_role: dict[str, Counter[str]] = field(default_factory=lambda: defaultdict(Counter))
    invalid_token_dictionary_issues: Counter[str] = field(default_factory=Counter)
    invalid_token_dictionary_issues_by_role: dict[str, Counter[str]] = field(
        default_factory=lambda: defaultdict(Counter)
    )
    invalid_token_dictionary_samples: list[dict[str, Any]] = field(default_factory=list)
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
        self.class_events[fact.fact_class] += 1
        self.role_events[fact.role] += 1
        for consumer in fact.consumers:
            self.consumer_events[consumer] += 1
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
        invalid_token_dictionary_issue_count = sum(self.invalid_token_dictionary_issues.values())
        route_error_events = (
            self.counts["state_model_consumer_on_non_state_fact"] + self.counts["unknown_state_model_role_events"]
        )
        seq_order_error_count = self._seq_order_error_count()
        return {
            "class_events": dict(sorted(self.class_events.items())),
            "role_events": dict(sorted(self.role_events.items())),
            "consumer_events": dict(sorted(self.consumer_events.items())),
            "workload_identity_event_count": self.class_events["workload_identity"],
            "hicache_state_model_event_count": self.consumer_events[HICACHE_CONSUMER_STATE_MODEL],
            "input_contract_event_count": self.consumer_events[HICACHE_CONSUMER_INPUT_CONTRACT],
            "required_events": self.counts["required_events"],
            "role_completed_events": dict(sorted(self.role_completed_events.items())),
            "missing_required_fact_events": self.counts["missing_required_fact_events"],
            "missing_fields": dict(sorted(self.missing_fields.items())),
            "missing_fields_by_role": {
                role: dict(sorted(counter.items())) for role, counter in sorted(self.missing_fields_by_role.items())
            },
            "route_error_events": route_error_events,
            "state_model_consumer_on_non_state_fact": self.counts["state_model_consumer_on_non_state_fact"],
            "unknown_state_model_role_events": self.counts["unknown_state_model_role_events"],
            "token_dictionary_paths": len(self.dictionary_ids),
            "token_dictionary_paths_with_token_ids": len(self.dictionary_ids_with_tokens),
            "token_span_refs": len(self.span_path_ids),
            "missing_token_dictionary_refs": missing_token_dictionary_refs,
            "dictionary_ids_without_tokens": dictionary_ids_without_tokens,
            "invalid_token_dictionary_issue_count": invalid_token_dictionary_issue_count,
            "invalid_token_dictionary_issues": dict(sorted(self.invalid_token_dictionary_issues.items())),
            "invalid_token_dictionary_issues_by_role": {
                role: dict(sorted(counter.items()))
                for role, counter in sorted(self.invalid_token_dictionary_issues_by_role.items())
            },
            "invalid_token_dictionary_samples": self.invalid_token_dictionary_samples,
            "seq_scope_count": len(self.seq_by_scope),
            "seq_order_error_count": seq_order_error_count,
            "ready": self._ready(
                route_error_events,
                missing_token_dictionary_refs,
                dictionary_ids_without_tokens,
                invalid_token_dictionary_issue_count,
                seq_order_error_count,
            ),
        }

    def _observe_unknown_role(self, args: dict[str, Any], role: str) -> None:
        self.counts["unknown_state_model_role_events"] += 1
        if completed_fact_role(args, role):
            self.counts["missing_required_fact_events"] += 1
            self.missing_fields["fact.role"] += 1

    def _observe_completed_required_fact(self, args: dict[str, Any], role: str) -> None:
        self.counts["required_events"] += 1
        self.role_completed_events[role] += 1
        missing = self._missing_fields(args, role)
        missing.extend(batch_state_fact_errors(args, role))
        if missing:
            self.counts["missing_required_fact_events"] += 1
            for field_name in missing:
                self.missing_fields[field_name] += 1
                self.missing_fields_by_role[role][field_name] += 1

        scope = args.get("cache_scope")
        seq_no = int_or_none(args.get("seq_no"))
        if has_fact(scope) and seq_no is not None:
            self.seq_by_scope[str(scope)].append(seq_no)

    def _observe_token_references(self, args: dict[str, Any], role: str) -> None:
        for field_name in STATE_FACT_DICTIONARY_FIELDS_BY_ROLE.get(role, ()):
            for item in fact_items(args.get(field_name)):
                if isinstance(item, dict):
                    self._observe_dictionary(role, field_name, item)
        for field_name in STATE_FACT_SPAN_FIELDS_BY_ROLE.get(role, ()):
            for item in fact_items(args.get(field_name)):
                if isinstance(item, dict):
                    path_id = item.get("path_id")
                    if isinstance(path_id, str) and path_id:
                        self.span_path_ids.add(path_id)

    def _observe_dictionary(self, role: str, field_name: str, item: dict[str, Any]) -> None:
        token_path_id = item.get("token_path_id")
        if not isinstance(token_path_id, str) or not token_path_id:
            return
        self.dictionary_ids.add(token_path_id)
        if isinstance(item.get("token_ids"), list):
            self.dictionary_ids_with_tokens.add(token_path_id)
            for issue in token_dictionary_issues(item):
                issue_name = str(issue.get("issue") or "token_dictionary_invalid")
                self.invalid_token_dictionary_issues[issue_name] += 1
                self.invalid_token_dictionary_issues_by_role[role][issue_name] += 1
                if len(self.invalid_token_dictionary_samples) < 8:
                    self.invalid_token_dictionary_samples.append({"role": role, "field": field_name, **issue})

    def _missing_fields(self, args: dict[str, Any], role: str) -> list[str]:
        missing = [field for field in STATE_FACT_REQUIRED_FIELDS_BY_ROLE.get(role, ()) if not has_fact(args.get(field))]
        for field_name in STATE_FACT_DICTIONARY_FIELDS_BY_ROLE.get(role, ()):
            items = fact_items(args.get(field_name))
            if args.get(field_name) is not None and (
                not items or any(not has_token_dictionary(item) for item in items)
            ):
                missing.append(f"{field_name}.token_path_id")
        for field_name in STATE_FACT_SPAN_FIELDS_BY_ROLE.get(role, ()):
            items = fact_items(args.get(field_name))
            if args.get(field_name) is not None and (not items or any(not has_token_span(item) for item in items)):
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
