"""Extraction and canonical signatures for workload-identity facts."""

from __future__ import annotations

import collections
import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ...core.facts import parse_fact_or_none
from .primitives import (
    batch_path_signatures,
    canonical_json,
    completed_workload_identity,
    optional_int,
    path_signature,
    request_anchor_signature_fields,
    scalar_value,
    trace_events,
)
from .types import (
    KNOWN_WORKLOAD_IDENTITY_ROLES,
    PATH_ROLES,
    REQUEST_SCOPED_ROLES,
    ROLE_SCALAR_FIELDS,
    AuditEvent,
)


@dataclass
class WorkloadIdentityExtractor:
    """Extract comparable workload-identity events from trace rows."""

    paths: list[Path]
    label: str
    roles: set[str]
    event_rows: list[tuple[Path, dict[str, Any]]] | None = None

    def extract(self) -> tuple[list[AuditEvent], collections.Counter[str], collections.Counter[str]]:
        """Return events plus unknown-role and unmapped-request counts."""

        rows: list[AuditEvent] = []
        unknown_roles: collections.Counter[str] = collections.Counter()
        unmapped_requests: collections.Counter[str] = collections.Counter()
        ordered = sorted(
            self.event_rows if self.event_rows is not None else trace_events(self.paths), key=self._event_sort_key
        )
        request_fingerprints = self._build_request_fingerprints(ordered)
        for _path, event in ordered:
            item = self._audit_event(event, rows, request_fingerprints, unknown_roles, unmapped_requests)
            if item is not None:
                rows.append(item)
        return rows, unknown_roles, unmapped_requests

    def _audit_event(
        self,
        event: dict[str, Any],
        rows: list[AuditEvent],
        request_fingerprints: dict[str, str],
        unknown_roles: collections.Counter[str],
        unmapped_requests: collections.Counter[str],
    ) -> AuditEvent | None:
        if not completed_workload_identity(event):
            return None
        args = event.get("args") if isinstance(event.get("args"), dict) else {}
        fact = parse_fact_or_none(args)
        if fact is None:
            return None
        role = fact.role
        if role not in KNOWN_WORKLOAD_IDENTITY_ROLES:
            unknown_roles[role or "missing_fact_role"] += 1
            return None
        if role not in self.roles:
            return None

        request_id = str(args.get("request_id") or "")
        request_fingerprint = self._request_fingerprint(role, request_id, request_fingerprints, unmapped_requests)
        self._audit_batch_mapping(role, args, request_fingerprints, unmapped_requests)
        signature, fields = build_signature(role, event, request_fingerprint, request_fingerprints)
        return AuditEvent(
            stream=self.label,
            ordinal=len(rows),
            role=role,
            signature=signature,
            target_id=str(args.get("target_id") or ""),
            event_name=str(event.get("name") or ""),
            ts=optional_int(event.get("ts")),
            seq_no=optional_int(args.get("seq_no")),
            request_id=request_id,
            request_fingerprint=request_fingerprint,
            cache_scope=str(args.get("cache_scope") or ""),
            fields=fields,
        )

    def _request_fingerprint(
        self,
        role: str,
        request_id: str,
        request_fingerprints: dict[str, str],
        unmapped_requests: collections.Counter[str],
    ) -> str:
        if role not in REQUEST_SCOPED_ROLES:
            return ""
        request_fingerprint = request_fingerprints.get(request_id, "") if request_id else ""
        if not request_fingerprint:
            unmapped_requests[role] += 1
            return "unmapped_request"
        return request_fingerprint

    def _audit_batch_mapping(
        self,
        role: str,
        args: dict[str, Any],
        request_fingerprints: dict[str, str],
        unmapped_requests: collections.Counter[str],
    ) -> None:
        if role != "cache_extend_input":
            return
        request_ids = args.get("request_ids")
        if isinstance(request_ids, str):
            try:
                request_ids = json.loads(request_ids)
            except json.JSONDecodeError:
                request_ids = []
        if isinstance(request_ids, list):
            for request_id in request_ids:
                if not request_fingerprints.get(str(request_id or "")):
                    unmapped_requests[role] += 1
                    break

    def _build_request_fingerprints(self, events: list[tuple[Path, dict[str, Any]]]) -> dict[str, str]:
        anchors_by_request: dict[str, collections.Counter[str]] = collections.defaultdict(collections.Counter)
        for _path, event in events:
            if not completed_workload_identity(event):
                continue
            args = event.get("args") if isinstance(event.get("args"), dict) else {}
            fact = parse_fact_or_none(args)
            if fact is None or fact.role not in PATH_ROLES:
                continue
            request_id = str(args.get("request_id") or "")
            if request_id:
                anchors_by_request[request_id][request_anchor_signature(fact.role, args)] += 1

        fingerprints: dict[str, str] = {}
        for request_id, anchors in anchors_by_request.items():
            payload = [{"count": count, "fact": json.loads(signature)} for signature, count in sorted(anchors.items())]
            encoded = canonical_json(payload).encode("utf-8")
            fingerprints[request_id] = "sha256_json:" + hashlib.sha256(encoded).hexdigest()
        return fingerprints

    @staticmethod
    def _event_sort_key(item: tuple[Path, dict[str, Any]]) -> tuple[int, str, int, str]:
        event = item[1]
        args = event.get("args") if isinstance(event.get("args"), dict) else {}
        return (
            optional_int(event.get("ts")),
            str(args.get("cache_scope") or ""),
            optional_int(args.get("seq_no")),
            str(event.get("name") or ""),
        )


def request_anchor_signature(role: str, args: dict[str, Any]) -> str:
    """Build the path-bearing anchor used for a request fingerprint."""

    return canonical_json(request_anchor_signature_fields(role, args))


def build_signature(
    role: str,
    event: dict[str, Any],
    request_fingerprint: str,
    request_fingerprints: dict[str, str] | None = None,
) -> tuple[str, dict[str, Any]]:
    """Build canonical fields and a signature for one identity fact."""

    args = event.get("args") if isinstance(event.get("args"), dict) else {}
    fields: dict[str, Any] = {"role": role}
    if role == "cache_extend_input":
        for field in ROLE_SCALAR_FIELDS.get(role, ()):
            value = scalar_value(args, field)
            if value is not None:
                fields[field] = value
        fields["batch_paths"] = batch_path_signatures(args, request_fingerprints or {})
        return canonical_json(fields), fields
    if role in REQUEST_SCOPED_ROLES:
        fields["request_fingerprint"] = request_fingerprint
    for field in ROLE_SCALAR_FIELDS.get(role, ()):
        value = scalar_value(args, field)
        if value is not None:
            fields[field] = value
    if role in PATH_ROLES:
        fields["path"] = path_signature(args)
    return canonical_json(fields), fields


def extract_audit_events(
    paths: list[Path],
    label: str,
    roles: set[str],
    *,
    event_rows: list[tuple[Path, dict[str, Any]]] | None = None,
) -> tuple[list[AuditEvent], collections.Counter[str], collections.Counter[str]]:
    """Extract comparable audit events from paths or preloaded trace rows."""

    return WorkloadIdentityExtractor(paths=paths, label=label, roles=roles, event_rows=event_rows).extract()
