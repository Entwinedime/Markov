"""Stateless parsing and canonicalization for workload signatures."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from markov_internal.common.trace import load_chrome_trace_events
from ...core.facts import HICACHE_CONSUMER_INPUT_CONTRACT, parse_fact_or_none
from ...core.tokens import maybe_json, token_id_path, token_path_count, token_path_hash
from .types import ROLE_SCALAR_FIELDS


def trace_events(paths: list[Path]) -> list[tuple[Path, dict[str, Any]]]:
    """Load event rows from one or more Chrome trace files."""

    rows: list[tuple[Path, dict[str, Any]]] = []
    for path in paths:
        events, _status = load_chrome_trace_events(path, auto_repair=True)
        rows.extend((path, event) for event in events)
    return rows


def optional_int(value: Any, default: int | None = 0) -> int | None:
    """Parse an integer and return ``default`` when conversion fails."""

    try:
        return int(float(value))
    except (TypeError, ValueError):
        return default


def maybe_int(value: Any) -> int | None:
    """Parse an integer permissively, returning ``None`` on failure."""

    try:
        return int(float(value))
    except (TypeError, ValueError):
        return None


def canonical_json(value: Any) -> str:
    """Serialize deterministic canonical JSON for a fact signature."""

    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))


def completed_workload_identity(event: dict[str, Any]) -> bool:
    """Return whether an event is the consumable phase of an identity fact."""

    args = event.get("args") if isinstance(event.get("args"), dict) else {}
    fact = parse_fact_or_none(args)
    if fact is None:
        return False
    if fact.fact_class != "workload_identity" or not fact.has_consumer(HICACHE_CONSUMER_INPUT_CONTRACT):
        return False
    phase = str(args.get("phase") or "").lower()
    name = str(event.get("name") or "")
    if fact.role == "cache_extend_input":
        return phase == "start" or name.endswith("_start")
    return phase == "end" or name.endswith("_end")


def dictionary_descriptor(args: dict[str, Any], key: str) -> dict[str, Any]:
    """Project a token dictionary to its cross-config identity fields."""

    value = maybe_json(args.get(key))
    if not isinstance(value, dict):
        return {}
    tokens = token_id_path(value)
    path_id = str(value.get("token_path_id") or value.get("path_id") or "")
    return {
        "path_id": path_id or (token_path_hash(tokens) if tokens else ""),
        "token_count": optional_int(
            value.get("token_count"),
            token_path_count(tokens) if tokens is not None else None,
        ),
        "hash_algo": str(value.get("hash_algo") or ""),
    }


def span_descriptor(args: dict[str, Any], key: str) -> dict[str, Any]:
    """Project a token span to its cross-config identity fields."""

    value = maybe_json(args.get(key))
    if not isinstance(value, dict):
        return {}
    begin = optional_int(value.get("begin"), 0) or 0
    end = optional_int(value.get("end"), 0) or 0
    return {
        "path_id": str(value.get("path_id") or value.get("token_path_id") or ""),
        "begin": begin,
        "end": end,
        "token_count": optional_int(value.get("token_count"), max(0, end - begin)),
        "hash_algo": str(value.get("hash_algo") or ""),
    }


def path_signature(args: dict[str, Any]) -> dict[str, Any]:
    """Combine dictionary and span descriptors into a stable path identity."""

    return {
        "dictionary": dictionary_descriptor(args, "token_dictionary"),
        "span": span_descriptor(args, "full_path_span"),
    }


def batch_path_signatures(args: dict[str, Any], fingerprints: dict[str, str] | None = None) -> list[dict[str, Any]]:
    """Project aligned ``cache_extend_input`` arrays to batch path identities."""

    request_ids = maybe_json(args.get("request_ids"))
    positions = maybe_json(args.get("request_positions"))
    dictionaries = maybe_json(args.get("token_dictionaries"))
    spans = maybe_json(args.get("full_path_spans"))
    token_counts = maybe_json(args.get("token_counts"))
    if not isinstance(request_ids, list):
        request_ids = []
    if not isinstance(positions, list):
        positions = []
    if not isinstance(dictionaries, list):
        dictionaries = []
    if not isinstance(spans, list):
        spans = []
    if not isinstance(token_counts, list):
        token_counts = []
    rows: list[dict[str, Any]] = []
    for index, request_id in enumerate(request_ids):
        request_id_text = str(request_id or "")
        position = positions[index] if index < len(positions) else {}
        dictionary = dictionaries[index] if index < len(dictionaries) else {}
        span = spans[index] if index < len(spans) else {}
        token_count = token_counts[index] if index < len(token_counts) else None
        rows.append(
            {
                "index": optional_int(position.get("index") if isinstance(position, dict) else index, index),
                "request_fingerprint": (fingerprints or {}).get(request_id_text, ""),
                "token_count": optional_int(token_count),
                "path": {
                    "dictionary": dictionary_descriptor({"token_dictionary": dictionary}, "token_dictionary"),
                    "span": span_descriptor({"full_path_span": span}, "full_path_span"),
                },
            }
        )
    return rows


def scalar_value(args: dict[str, Any], field: str) -> Any:
    """Read a scalar or structured field admitted to a signature."""

    value = maybe_json(args.get(field))
    if isinstance(value, (dict, list)):
        return value
    if value is None:
        return None
    return str(value)


def request_anchor_signature_fields(role: str, args: dict[str, Any]) -> dict[str, Any]:
    """Build canonical fields for a request-fingerprint anchor."""

    fields: dict[str, Any] = {"role": role}
    for field in ROLE_SCALAR_FIELDS.get(role, ()):
        value = scalar_value(args, field)
        if value is not None:
            fields[field] = value
    fields["path"] = path_signature(args)
    return fields
