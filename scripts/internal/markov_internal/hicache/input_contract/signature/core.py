"""HiCache 输入合同 fact 抽取与 canonical signature 生成。"""

from __future__ import annotations

import collections
import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ....common.trace import load_chrome_trace_events
from ...core.facts import HICACHE_CONSUMER_INPUT_CONTRACT, parse_fact_or_none
from ...core.tokens import (
    token_dictionary_issues,
    token_id_path,
    token_path_count,
    token_path_hash,
)


DEFAULT_ROLES = (
    "cache_lookup_input",
    "cache_extend_input",
    "cache_lifecycle_commit",
    "prefetch_candidate_anchor",
)

PATH_ROLES = {
    "cache_lookup_input",
    "cache_extend_input",
    "cache_lifecycle_commit",
    "prefetch_candidate_anchor",
}

ROLE_SCALAR_FIELDS = {
    "cache_lookup_input": ("token_count",),
    "cache_lifecycle_commit": ("lifecycle_kind", "token_count", "priority"),
    "prefetch_candidate_anchor": ("token_count",),
    "cache_extend_input": ("batch_kind", "batch_size"),
}

KNOWN_WORKLOAD_IDENTITY_ROLES = set(DEFAULT_ROLES)
REQUEST_SCOPED_ROLES = {
    "cache_lookup_input",
    "cache_lifecycle_commit",
    "prefetch_candidate_anchor",
}


@dataclass(frozen=True)
class AuditEvent:
    """可比较的 workload identity fact 摘要。"""

    stream: str
    ordinal: int
    role: str
    signature: str
    target_id: str
    event_name: str
    ts: int
    seq_no: int
    request_id: str
    request_fingerprint: str
    cache_scope: str
    fields: dict[str, Any]


def trace_events(paths: list[Path]) -> list[tuple[Path, dict[str, Any]]]:
    """从一个或多个 Chrome trace 文件中提取 event 行。"""

    rows: list[tuple[Path, dict[str, Any]]] = []
    for path in paths:
        events, _status = load_chrome_trace_events(path, auto_repair=True)
        rows.extend((path, event) for event in events)
    return rows


def optional_int(value: Any, default: int = 0) -> int:
    """宽松解析整数，失败时返回 default。"""

    try:
        return int(float(value))
    except (TypeError, ValueError):
        return default


def maybe_int(value: Any) -> int | None:
    """宽松解析整数，失败时返回 None。"""

    try:
        return int(float(value))
    except (TypeError, ValueError):
        return None


def maybe_json(value: Any) -> Any:
    """解析可能被双重 JSON 编码的 trace arg。"""

    if not isinstance(value, str):
        return value
    text = value.strip()
    if not text:
        return value
    try:
        parsed = json.loads(text)
        if isinstance(parsed, str):
            nested = parsed.strip()
            if nested.startswith("{") or nested.startswith("["):
                return json.loads(nested)
        return parsed
    except json.JSONDecodeError:
        return value


def canonical_json(value: Any) -> str:
    """生成稳定 canonical JSON 字符串，用作 fact signature。"""

    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))


def completed_workload_identity(event: dict[str, Any]) -> bool:
    """判断 event 是否是已完成的 workload identity input-contract fact。"""

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
    """提取 token dictionary 的跨配置比较描述。"""

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
    """提取 token span 的跨配置比较描述。"""

    value = maybe_json(args.get(key))
    if not isinstance(value, dict):
        return {}
    begin = optional_int(value.get("begin"))
    end = optional_int(value.get("end"))
    return {
        "path_id": str(value.get("path_id") or value.get("token_path_id") or ""),
        "begin": begin,
        "end": end,
        "token_count": optional_int(value.get("token_count"), max(0, end - begin)),
        "hash_algo": str(value.get("hash_algo") or ""),
    }


def path_signature(args: dict[str, Any]) -> dict[str, Any]:
    """组合 dictionary/span，形成 path-bearing fact 的稳定描述。"""

    return {
        "dictionary": dictionary_descriptor(args, "token_dictionary"),
        "span": span_descriptor(args, "full_path_span"),
    }


def batch_path_signatures(args: dict[str, Any], fingerprints: dict[str, str] | None = None) -> list[dict[str, Any]]:
    """组合 cache_extend_input 的 batch path 数组。"""

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


def workload_identity_path_contract(paths: list[Path], roles: set[str], sample: int) -> dict[str, Any]:
    """检查 path-bearing workload identity event 是否能被 C++ token parser 直接消费。"""

    issue_counts: collections.Counter[str] = collections.Counter()
    issue_counts_by_role: collections.Counter[str] = collections.Counter()
    samples: list[dict[str, Any]] = []
    path_refs_by_id: dict[str, set[str]] = collections.defaultdict(set)
    path_ids_with_tokens: set[str] = set()
    path_event_count = 0

    def record_issue(role: str, issue: str, event: dict[str, Any], detail: dict[str, Any] | None = None) -> None:
        """记录 path-bearing fact 的合同缺口，并保留有限样本。"""

        issue_counts[issue] += 1
        issue_counts_by_role[role] += 1
        if len(samples) >= sample:
            return
        args = event.get("args") if isinstance(event.get("args"), dict) else {}
        samples.append(
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

    events = trace_events(paths)
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
                    path_ids_with_tokens.add(path_id)

    for _path, event in events:
        if not completed_workload_identity(event):
            continue
        args = event.get("args") if isinstance(event.get("args"), dict) else {}
        fact = parse_fact_or_none(args)
        if fact is None:
            continue
        role = fact.role
        if role not in roles or role not in PATH_ROLES:
            continue

        path_event_count += 1
        if role == "cache_extend_input":
            request_ids = maybe_json(args.get("request_ids"))
            request_positions = maybe_json(args.get("request_positions"))
            token_dictionaries = maybe_json(args.get("token_dictionaries"))
            full_path_spans = maybe_json(args.get("full_path_spans"))
            token_counts = maybe_json(args.get("token_counts"))
            batch_size = maybe_int(args.get("batch_size"))
            if not isinstance(request_ids, list) or not request_ids:
                record_issue(role, "missing_batch_request_ids", event)
                request_ids = []
            expected = len(request_ids)
            for field_name, value in (
                ("token_dictionaries", token_dictionaries),
                ("full_path_spans", full_path_spans),
                ("token_counts", token_counts),
            ):
                if not isinstance(value, list):
                    record_issue(role, f"missing_{field_name}", event)
                elif len(value) != expected:
                    record_issue(
                        role, f"{field_name}_length_mismatch", event, {"expected": expected, "actual": len(value)}
                    )
            if not isinstance(request_positions, list):
                record_issue(role, "missing_request_positions", event)
            elif len(request_positions) != expected:
                record_issue(
                    role,
                    "request_positions_length_mismatch",
                    event,
                    {"expected": expected, "actual": len(request_positions)},
                )
            else:
                indexes: list[int] = []
                string_ids = [str(item) for item in request_ids if item is not None]
                for position in request_positions:
                    if not isinstance(position, dict):
                        record_issue(role, "request_positions_item_invalid", event)
                        continue
                    index = maybe_int(position.get("index"))
                    if index is None:
                        record_issue(role, "request_positions_index_missing", event)
                        continue
                    indexes.append(index)
                    row_request_id = str(position.get("request_id") or "")
                    if 0 <= index < expected and row_request_id and row_request_id != string_ids[index]:
                        record_issue(
                            role,
                            "request_positions_request_id_mismatch",
                            event,
                            {"index": index, "expected": string_ids[index], "actual": row_request_id},
                        )
                if sorted(indexes) != list(range(expected)):
                    record_issue(
                        role,
                        "request_positions_coverage_mismatch",
                        event,
                        {"expected": expected, "actual": sorted(indexes)},
                    )
            if batch_size != expected:
                record_issue(role, "batch_size_mismatch", event, {"expected": expected, "actual": batch_size})
            string_ids = [str(item) for item in request_ids if item is not None]
            if len(string_ids) != expected or any(not item for item in string_ids):
                record_issue(role, "request_ids_invalid", event)
            elif len(set(string_ids)) != len(string_ids):
                record_issue(role, "duplicate_request_ids", event)
            dictionaries = token_dictionaries if isinstance(token_dictionaries, list) else []
            spans = full_path_spans if isinstance(full_path_spans, list) else []
            counts = token_counts if isinstance(token_counts, list) else []
            for index in range(expected):
                token_count = maybe_int(counts[index]) if index < len(counts) else None
                if token_count is None:
                    record_issue(role, "missing_role_token_count", event, {"index": index})
                dictionary = dictionaries[index] if index < len(dictionaries) else None
                span = spans[index] if index < len(spans) else None
                _validate_path_reference(
                    role, event, dictionary, span, record_issue, path_refs_by_id, path_ids_with_tokens
                )
            continue

        dictionary = maybe_json(args.get("token_dictionary"))
        span = maybe_json(args.get("full_path_span"))
        if maybe_int(args.get("token_count")) is None:
            record_issue(role, "missing_role_token_count", event)
        _validate_path_reference(role, event, dictionary, span, record_issue, path_refs_by_id, path_ids_with_tokens)

    missing_token_ids = sorted(path_id for path_id in path_refs_by_id if path_id not in path_ids_with_tokens)
    for path_id in missing_token_ids:
        roles_for_path = sorted(path_refs_by_id[path_id])
        issue_counts["token_dictionary_missing_token_ids"] += 1
        for role in roles_for_path:
            issue_counts_by_role[role] += 1
        if len(samples) < sample:
            samples.append(
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

    issue_count = sum(issue_counts.values())
    return {
        "ready": issue_count == 0,
        "path_event_count": path_event_count,
        "referenced_path_count": len(path_refs_by_id),
        "path_ids_with_token_ids": len(path_ids_with_tokens),
        "missing_token_ids_path_count": len(missing_token_ids),
        "issue_count": issue_count,
        "issue_counts": dict(sorted(issue_counts.items())),
        "issue_counts_by_role": dict(sorted(issue_counts_by_role.items())),
        "blocking_roles": sorted(issue_counts_by_role),
        "samples": samples,
    }


def _validate_path_reference(
    role: str,
    event: dict[str, Any],
    dictionary: Any,
    span: Any,
    record_issue: Any,
    path_refs_by_id: dict[str, set[str]],
    path_ids_with_tokens: set[str],
) -> None:
    """检查一组 token dictionary/span 引用。"""

    dictionary_path_id = ""
    dictionary_token_count: int | None = None
    if not isinstance(dictionary, dict):
        record_issue(role, "missing_token_dictionary", event)
    else:
        dictionary_path_id = str(dictionary.get("token_path_id") or dictionary.get("path_id") or "")
        dictionary_token_count = maybe_int(dictionary.get("token_count"))
        if not dictionary_path_id:
            record_issue(role, "missing_token_dictionary_path_id", event)
        if dictionary_token_count is None:
            record_issue(role, "missing_token_dictionary_token_count", event, {"path_id": dictionary_path_id})
        if not str(dictionary.get("hash_algo") or ""):
            record_issue(role, "missing_token_dictionary_hash_algo", event, {"path_id": dictionary_path_id})

        tokens = token_id_path(dictionary)
        if tokens is not None and dictionary_path_id:
            path_ids_with_tokens.add(dictionary_path_id)
        for issue in token_dictionary_issues(dictionary):
            record_issue(role, str(issue.get("issue") or "token_dictionary_invalid"), event, issue)

    span_path_id = ""
    span_begin: int | None = None
    span_end: int | None = None
    if not isinstance(span, dict):
        record_issue(role, "missing_full_path_span", event, {"path_id": dictionary_path_id})
    else:
        span_path_id = str(span.get("path_id") or span.get("token_path_id") or "")
        span_begin = maybe_int(span.get("begin"))
        span_end = maybe_int(span.get("end"))
        if not span_path_id:
            record_issue(role, "missing_full_path_span_path_id", event, {"path_id": dictionary_path_id})
        if span_begin is None:
            record_issue(role, "missing_full_path_span_begin", event, {"path_id": span_path_id})
        if span_end is None:
            record_issue(role, "missing_full_path_span_end", event, {"path_id": span_path_id})
        if not str(span.get("hash_algo") or ""):
            record_issue(role, "missing_full_path_span_hash_algo", event, {"path_id": span_path_id})
        if span_begin is not None and span_end is not None and span_end < span_begin:
            record_issue(
                role,
                "invalid_full_path_span_range",
                event,
                {"path_id": span_path_id, "begin": span_begin, "end": span_end},
            )
        if dictionary_path_id and span_path_id and dictionary_path_id != span_path_id:
            record_issue(
                role,
                "dictionary_span_path_mismatch",
                event,
                {"dictionary_path_id": dictionary_path_id, "span_path_id": span_path_id},
            )
        if dictionary_token_count is not None and span_end is not None and span_end > dictionary_token_count:
            record_issue(
                role,
                "full_path_span_exceeds_dictionary",
                event,
                {"path_id": span_path_id, "span_end": span_end, "dictionary_token_count": dictionary_token_count},
            )

    for path_id in {dictionary_path_id, span_path_id} - {""}:
        path_refs_by_id[path_id].add(role)


def fact_items(value: Any) -> list[Any]:
    """把 scalar fact 字段和数组 fact 字段统一成列表。"""

    if isinstance(value, list):
        return value
    if value is None:
        return []
    return [value]


def request_anchor_signature(role: str, args: dict[str, Any]) -> str:
    """生成 request fingerprint 使用的 path-bearing anchor signature。"""

    fields: dict[str, Any] = {"role": role}
    for field in ROLE_SCALAR_FIELDS.get(role, ()):
        value = scalar_value(args, field)
        if value is not None:
            fields[field] = value
    fields["path"] = path_signature(args)
    return canonical_json(fields)


def build_request_fingerprints(events: list[tuple[Path, dict[str, Any]]]) -> dict[str, str]:
    """把 run-local request id 映射成 path fact 派生的稳定 fingerprint。"""

    anchors_by_request: dict[str, collections.Counter[str]] = collections.defaultdict(collections.Counter)
    for _path, event in events:
        if not completed_workload_identity(event):
            continue
        args = event.get("args") if isinstance(event.get("args"), dict) else {}
        fact = parse_fact_or_none(args)
        if fact is None:
            continue
        role = fact.role
        if role not in PATH_ROLES:
            continue
        request_id = str(args.get("request_id") or "")
        if not request_id:
            continue
        anchors_by_request[request_id][request_anchor_signature(role, args)] += 1

    fingerprints: dict[str, str] = {}
    for request_id, anchors in anchors_by_request.items():
        payload = [{"count": count, "fact": json.loads(signature)} for signature, count in sorted(anchors.items())]
        encoded = canonical_json(payload).encode("utf-8")
        fingerprints[request_id] = "sha256_json:" + hashlib.sha256(encoded).hexdigest()
    return fingerprints


def scalar_value(args: dict[str, Any], field: str) -> Any:
    """读取 signature 中允许参与比较的标量或结构化字段。"""

    value = maybe_json(args.get(field))
    if isinstance(value, (dict, list)):
        return value
    if value is None:
        return None
    return str(value)


def build_signature(
    role: str,
    event: dict[str, Any],
    request_fingerprint: str,
    request_fingerprints: dict[str, str] | None = None,
) -> tuple[str, dict[str, Any]]:
    """为单个 workload identity fact 构造 canonical signature。"""

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
) -> tuple[list[AuditEvent], collections.Counter[str], collections.Counter[str]]:
    """从 trace 中抽取可比较的 AuditEvent 列表。"""

    rows: list[AuditEvent] = []
    unknown_workload_identity_roles: collections.Counter[str] = collections.Counter()
    unmapped_request_id_events: collections.Counter[str] = collections.Counter()
    ordered = sorted(
        trace_events(paths),
        key=lambda item: (
            optional_int(item[1].get("ts")),
            str((item[1].get("args") if isinstance(item[1].get("args"), dict) else {}).get("cache_scope") or ""),
            optional_int((item[1].get("args") if isinstance(item[1].get("args"), dict) else {}).get("seq_no")),
            str(item[1].get("name") or ""),
        ),
    )
    request_fingerprints = build_request_fingerprints(ordered)
    for _path, event in ordered:
        if not completed_workload_identity(event):
            continue
        args = event.get("args") if isinstance(event.get("args"), dict) else {}
        fact = parse_fact_or_none(args)
        if fact is None:
            continue
        role = fact.role
        if role not in KNOWN_WORKLOAD_IDENTITY_ROLES:
            unknown_workload_identity_roles[role or "missing_fact_role"] += 1
            continue
        if role not in roles:
            continue
        request_id = str(args.get("request_id") or "")
        request_fingerprint = ""
        if role in REQUEST_SCOPED_ROLES:
            if request_id:
                request_fingerprint = request_fingerprints.get(request_id, "")
            if not request_fingerprint:
                unmapped_request_id_events[role] += 1
                request_fingerprint = "unmapped_request"
        if role == "cache_extend_input":
            request_ids = maybe_json(args.get("request_ids"))
            if isinstance(request_ids, list):
                for batch_request_id in request_ids:
                    if not request_fingerprints.get(str(batch_request_id or "")):
                        unmapped_request_id_events[role] += 1
                        break
        signature, fields = build_signature(role, event, request_fingerprint, request_fingerprints)
        rows.append(
            AuditEvent(
                stream=label,
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
        )
    return rows, unknown_workload_identity_roles, unmapped_request_id_events
