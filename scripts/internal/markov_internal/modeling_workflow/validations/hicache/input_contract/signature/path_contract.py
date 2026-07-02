"""workload identity 中携带 path 的 fact 合同审计。"""

from __future__ import annotations

import collections
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from ...core.facts import parse_fact_or_none
from ...core.tokens import token_dictionary_issues, token_id_path
from .primitives import completed_workload_identity, fact_items, maybe_int, maybe_json, optional_int, trace_events
from .types import PATH_ROLES


@dataclass
class TokenPathContractAuditor:
    """检查 C++ token parser 可直接消费的 path-bearing workload fact。"""

    paths: list[Path]
    roles: set[str]
    sample_limit: int
    issue_counts: collections.Counter[str] = field(default_factory=collections.Counter)
    issue_counts_by_role: collections.Counter[str] = field(default_factory=collections.Counter)
    samples: list[dict[str, Any]] = field(default_factory=list)
    path_refs_by_id: dict[str, set[str]] = field(default_factory=lambda: collections.defaultdict(set))
    path_ids_with_tokens: set[str] = field(default_factory=set)
    path_event_count: int = 0

    def audit(self) -> dict[str, Any]:
        """执行 path 合同审计并返回 summary。"""

        events = trace_events(self.paths)
        self._collect_dictionary_payloads(events)
        for _path, event in events:
            self._audit_event(event)
        return self._summary()

    def _collect_dictionary_payloads(self, events: list[tuple[Path, dict[str, Any]]]) -> None:
        for _path, event in events:
            if not completed_workload_identity(event):
                continue
            args = event.get("args") if isinstance(event.get("args"), dict) else {}
            for key, value in args.items():
                if "dictionary" not in str(key):
                    continue
                for dictionary in fact_items(maybe_json(value)):
                    if not isinstance(dictionary, dict):
                        continue
                    path_id = str(dictionary.get("token_path_id") or dictionary.get("path_id") or "")
                    tokens = token_id_path(dictionary)
                    if path_id and tokens is not None:
                        self.path_ids_with_tokens.add(path_id)

    def _audit_event(self, event: dict[str, Any]) -> None:
        if not completed_workload_identity(event):
            return
        args = event.get("args") if isinstance(event.get("args"), dict) else {}
        fact = parse_fact_or_none(args)
        if fact is None:
            return
        role = fact.role
        if role not in self.roles or role not in PATH_ROLES:
            return
        self.path_event_count += 1
        if role == "cache_extend_input":
            self._audit_batch_event(role, event, args)
            return
        dictionary = maybe_json(args.get("token_dictionary"))
        span = maybe_json(args.get("full_path_span"))
        if maybe_int(args.get("token_count")) is None:
            self._record_issue(role, "missing_role_token_count", event)
        self._validate_path_reference(role, event, dictionary, span)

    def _audit_batch_event(self, role: str, event: dict[str, Any], args: dict[str, Any]) -> None:
        request_ids = maybe_json(args.get("request_ids"))
        request_positions = maybe_json(args.get("request_positions"))
        token_dictionaries = maybe_json(args.get("token_dictionaries"))
        full_path_spans = maybe_json(args.get("full_path_spans"))
        token_counts = maybe_json(args.get("token_counts"))
        batch_size = maybe_int(args.get("batch_size"))
        if not isinstance(request_ids, list) or not request_ids:
            self._record_issue(role, "missing_batch_request_ids", event)
            request_ids = []
        expected = len(request_ids)
        self._audit_batch_arrays(role, event, expected, token_dictionaries, full_path_spans, token_counts)
        self._audit_batch_positions(role, event, expected, request_ids, request_positions)
        if batch_size != expected:
            self._record_issue(role, "batch_size_mismatch", event, {"expected": expected, "actual": batch_size})
        string_ids = [str(item) for item in request_ids if item is not None]
        if len(string_ids) != expected or any(not item for item in string_ids):
            self._record_issue(role, "request_ids_invalid", event)
        elif len(set(string_ids)) != len(string_ids):
            self._record_issue(role, "duplicate_request_ids", event)

        dictionaries = token_dictionaries if isinstance(token_dictionaries, list) else []
        spans = full_path_spans if isinstance(full_path_spans, list) else []
        counts = token_counts if isinstance(token_counts, list) else []
        for index in range(expected):
            token_count = maybe_int(counts[index]) if index < len(counts) else None
            if token_count is None:
                self._record_issue(role, "missing_role_token_count", event, {"index": index})
            dictionary = dictionaries[index] if index < len(dictionaries) else None
            span = spans[index] if index < len(spans) else None
            self._validate_path_reference(role, event, dictionary, span)

    def _audit_batch_arrays(
        self,
        role: str,
        event: dict[str, Any],
        expected: int,
        token_dictionaries: Any,
        full_path_spans: Any,
        token_counts: Any,
    ) -> None:
        for field_name, value in (
            ("token_dictionaries", token_dictionaries),
            ("full_path_spans", full_path_spans),
            ("token_counts", token_counts),
        ):
            if not isinstance(value, list):
                self._record_issue(role, f"missing_{field_name}", event)
            elif len(value) != expected:
                self._record_issue(
                    role, f"{field_name}_length_mismatch", event, {"expected": expected, "actual": len(value)}
                )

    def _audit_batch_positions(
        self,
        role: str,
        event: dict[str, Any],
        expected: int,
        request_ids: list[Any],
        request_positions: Any,
    ) -> None:
        if not isinstance(request_positions, list):
            self._record_issue(role, "missing_request_positions", event)
            return
        if len(request_positions) != expected:
            self._record_issue(
                role,
                "request_positions_length_mismatch",
                event,
                {"expected": expected, "actual": len(request_positions)},
            )
            return
        indexes: list[int] = []
        string_ids = [str(item) for item in request_ids if item is not None]
        for position in request_positions:
            if not isinstance(position, dict):
                self._record_issue(role, "request_positions_item_invalid", event)
                continue
            index = maybe_int(position.get("index"))
            if index is None:
                self._record_issue(role, "request_positions_index_missing", event)
                continue
            indexes.append(index)
            row_request_id = str(position.get("request_id") or "")
            if 0 <= index < expected and row_request_id and row_request_id != string_ids[index]:
                self._record_issue(
                    role,
                    "request_positions_request_id_mismatch",
                    event,
                    {"index": index, "expected": string_ids[index], "actual": row_request_id},
                )
        if sorted(indexes) != list(range(expected)):
            self._record_issue(
                role,
                "request_positions_coverage_mismatch",
                event,
                {"expected": expected, "actual": sorted(indexes)},
            )

    def _validate_path_reference(self, role: str, event: dict[str, Any], dictionary: Any, span: Any) -> None:
        dictionary_path_id = ""
        dictionary_token_count: int | None = None
        if not isinstance(dictionary, dict):
            self._record_issue(role, "missing_token_dictionary", event)
        else:
            dictionary_path_id = str(dictionary.get("token_path_id") or dictionary.get("path_id") or "")
            dictionary_token_count = maybe_int(dictionary.get("token_count"))
            if not dictionary_path_id:
                self._record_issue(role, "missing_token_dictionary_path_id", event)
            if dictionary_token_count is None:
                self._record_issue(role, "missing_token_dictionary_token_count", event, {"path_id": dictionary_path_id})
            if not str(dictionary.get("hash_algo") or ""):
                self._record_issue(role, "missing_token_dictionary_hash_algo", event, {"path_id": dictionary_path_id})
            tokens = token_id_path(dictionary)
            if tokens is not None and dictionary_path_id:
                self.path_ids_with_tokens.add(dictionary_path_id)
            for issue in token_dictionary_issues(dictionary):
                self._record_issue(role, str(issue.get("issue") or "token_dictionary_invalid"), event, issue)

        span_path_id = self._validate_span(role, event, span, dictionary_path_id, dictionary_token_count)
        for path_id in {dictionary_path_id, span_path_id} - {""}:
            self.path_refs_by_id[path_id].add(role)

    def _validate_span(
        self,
        role: str,
        event: dict[str, Any],
        span: Any,
        dictionary_path_id: str,
        dictionary_token_count: int | None,
    ) -> str:
        if not isinstance(span, dict):
            self._record_issue(role, "missing_full_path_span", event, {"path_id": dictionary_path_id})
            return ""
        span_path_id = str(span.get("path_id") or span.get("token_path_id") or "")
        span_begin = maybe_int(span.get("begin"))
        span_end = maybe_int(span.get("end"))
        if not span_path_id:
            self._record_issue(role, "missing_full_path_span_path_id", event, {"path_id": dictionary_path_id})
        if span_begin is None:
            self._record_issue(role, "missing_full_path_span_begin", event, {"path_id": span_path_id})
        if span_end is None:
            self._record_issue(role, "missing_full_path_span_end", event, {"path_id": span_path_id})
        if not str(span.get("hash_algo") or ""):
            self._record_issue(role, "missing_full_path_span_hash_algo", event, {"path_id": span_path_id})
        if span_begin is not None and span_end is not None and span_end < span_begin:
            self._record_issue(
                role,
                "invalid_full_path_span_range",
                event,
                {"path_id": span_path_id, "begin": span_begin, "end": span_end},
            )
        if dictionary_path_id and span_path_id and dictionary_path_id != span_path_id:
            self._record_issue(
                role,
                "dictionary_span_path_mismatch",
                event,
                {"dictionary_path_id": dictionary_path_id, "span_path_id": span_path_id},
            )
        if dictionary_token_count is not None and span_end is not None and span_end > dictionary_token_count:
            self._record_issue(
                role,
                "full_path_span_exceeds_dictionary",
                event,
                {"path_id": span_path_id, "span_end": span_end, "dictionary_token_count": dictionary_token_count},
            )
        return span_path_id

    def _record_issue(
        self,
        role: str,
        issue: str,
        event: dict[str, Any],
        detail: dict[str, Any] | None = None,
    ) -> None:
        self.issue_counts[issue] += 1
        self.issue_counts_by_role[role] += 1
        if len(self.samples) >= self.sample_limit:
            return
        args = event.get("args") if isinstance(event.get("args"), dict) else {}
        self.samples.append(
            {
                "role": role,
                "issue": issue,
                "event_name": str(event.get("name") or ""),
                "target_id": str(args.get("target_id") or ""),
                "request_id": str(args.get("request_id") or ""),
                "seq_no": optional_int(args.get("seq_no")),
                "detail": detail or {},
            }
        )

    def _summary(self) -> dict[str, Any]:
        missing_token_ids = sorted(
            path_id for path_id in self.path_refs_by_id if path_id not in self.path_ids_with_tokens
        )
        for path_id in missing_token_ids:
            roles_for_path = sorted(self.path_refs_by_id[path_id])
            self.issue_counts["token_dictionary_missing_token_ids"] += 1
            for role in roles_for_path:
                self.issue_counts_by_role[role] += 1
            if len(self.samples) < self.sample_limit:
                self.samples.append(
                    {
                        "role": ",".join(roles_for_path),
                        "issue": "token_dictionary_missing_token_ids",
                        "event_name": "",
                        "target_id": "",
                        "request_id": "",
                        "seq_no": 0,
                        "detail": {"path_id": path_id, "referenced_by_roles": roles_for_path},
                    }
                )

        issue_count = sum(self.issue_counts.values())
        return {
            "ready": issue_count == 0,
            "path_event_count": self.path_event_count,
            "referenced_path_count": len(self.path_refs_by_id),
            "path_ids_with_token_ids": len(self.path_ids_with_tokens),
            "missing_token_ids_path_count": len(missing_token_ids),
            "issue_count": issue_count,
            "issue_counts": dict(sorted(self.issue_counts.items())),
            "issue_counts_by_role": dict(sorted(self.issue_counts_by_role.items())),
            "blocking_roles": sorted(self.issue_counts_by_role),
            "samples": self.samples,
        }


def workload_identity_path_contract(paths: list[Path], roles: set[str], sample: int) -> dict[str, Any]:
    """检查 path-bearing workload identity event 是否满足 token path 合同。"""

    return TokenPathContractAuditor(paths=paths, roles=roles, sample_limit=sample).audit()
