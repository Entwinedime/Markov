#!/usr/bin/env python3
"""跨配置审计 HiCache atomic invariant model-input facts。

该脚本只读真实 trace，比较 source/target 中已声明为 model_input 的 atomic invariant
事实集合是否在 request 归一化后匹配。它不生成 synthetic fact，也不把 target actual
或 oracle 信息写回模型输入。
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from trace_json import load_chrome_trace_events  # noqa: E402


DEFAULT_ROLES = (
    "request_bound_match_anchor",
    "request_lifecycle_anchor",
    "request_admission",
    "prefetch_decision",
    "prefetch_check_point",
)

PATH_ROLES = {
    "request_bound_match_anchor",
    "request_lifecycle_anchor",
    "request_admission",
    "prefetch_decision",
}

ROLE_SCALAR_FIELDS = {
    "request_bound_match_anchor": ("token_count",),
    "request_lifecycle_anchor": ("lifecycle_kind", "token_count", "is_insert", "chunked", "priority"),
    "request_admission": (
        "admission_kind",
        "token_count",
        "has_chunked_req",
        "truncation_align_size",
        "priority",
        "ignore_eos",
        "max_new_tokens",
    ),
    "prefetch_decision": ("token_count",),
    "prefetch_check_point": ("check_kind",),
}

KNOWN_ATOMIC_INVARIANT_ROLES = set(DEFAULT_ROLES)
REQUEST_SCOPED_ROLES = set(DEFAULT_ROLES)


@dataclass(frozen=True)
class AuditEvent:
    """可比较的 atomic invariant fact 摘要。

    signature 是跨配置比较的 canonical key；原始 request_id 只保留为诊断字段，
    不直接参与 source/target 匹配。
    """

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


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """解析 cross-input audit CLI 参数。"""

    parser = argparse.ArgumentParser(
        description="Audit cross-config HiCache atomic invariant model-input facts."
    )
    parser.add_argument(
        "--source-trace",
        type=Path,
        action="append",
        required=True,
        help="Source Python probe trace path. Can be repeated.",
    )
    parser.add_argument(
        "--target-trace",
        type=Path,
        action="append",
        required=True,
        help="Target Python probe trace path. Can be repeated.",
    )
    parser.add_argument("--source-label", default="source", help="Source label used in the report.")
    parser.add_argument("--target-label", default="target", help="Target label used in the report.")
    parser.add_argument("--role", action="append", default=[], help="Invariant role to audit. Can be repeated.")
    parser.add_argument("--output", type=Path, help="Output JSON path. Prints full report when omitted.")
    parser.add_argument("--sample", type=int, default=8, help="Maximum mismatch / issue sample size.")
    return parser.parse_args(argv)


def load_json(path: Path) -> Any:
    """读取 JSON 文件。"""

    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value: Any) -> None:
    """写入 JSON 文件并创建父目录。"""

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


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


def true_like(value: Any) -> bool:
    """解析 trace args 中可能出现的宽松布尔值。"""

    if isinstance(value, bool):
        return value
    if value is None:
        return False
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return value.strip().lower() in {"1", "true", "yes", "on"}
    return False


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


def completed_model_invariant(event: dict[str, Any]) -> bool:
    """判断 event 是否是已完成的 atomic invariant model-input fact。"""

    args = event.get("args") if isinstance(event.get("args"), dict) else {}
    if not true_like(args.get("model_input")):
        return False
    if str(args.get("fact_class") or "") != "invariant_state":
        return False
    if str(args.get("fact_granularity") or "") != "atomic":
        return False
    phase = str(args.get("phase") or "").lower()
    name = str(event.get("name") or "")
    return phase == "end" or name.endswith("_end")


def token_ids(value: Any) -> tuple[int, ...]:
    """从 token dictionary JSON 中提取 token id 序列。"""

    tokens = token_id_list(value)
    return () if tokens is None else tokens


def token_id_list(value: Any) -> tuple[int, ...] | None:
    """从 token dictionary JSON 中提取 token id 序列，缺失时返回 None。"""

    value = maybe_json(value)
    if not isinstance(value, dict):
        return None
    raw_tokens = value.get("token_ids")
    if not isinstance(raw_tokens, list):
        return None
    parsed: list[int] = []
    for token in raw_tokens:
        try:
            parsed.append(int(token[0] if isinstance(token, list) else token))
        except (TypeError, ValueError, IndexError):
            return None
    return tuple(parsed)


def token_hash(tokens: tuple[int, ...]) -> str:
    """按 u32 little-endian token 序列生成稳定 path hash。"""

    hasher = hashlib.sha256()
    for token in tokens:
        hasher.update(int(token).to_bytes(4, byteorder="little", signed=False))
    return "sha256_u32le:" + hasher.hexdigest()


def dictionary_descriptor(args: dict[str, Any], key: str) -> dict[str, Any]:
    """提取 token dictionary 的跨配置比较描述。"""

    value = maybe_json(args.get(key))
    if not isinstance(value, dict):
        return {}
    tokens = token_ids(value)
    path_id = str(value.get("token_path_id") or value.get("path_id") or "")
    return {
        "path_id": path_id or (token_hash(tokens) if tokens else ""),
        "token_count": optional_int(value.get("token_count"), len(tokens)),
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

    dictionary = dictionary_descriptor(args, "token_dictionary")
    span = span_descriptor(args, "full_path_span")
    return {
        "dictionary": dictionary,
        "span": span,
    }


def model_input_path_contract(paths: list[Path], roles: set[str], sample: int) -> dict[str, Any]:
    """检查 path-bearing invariant event 是否能被 C++ token parser 直接消费。"""

    issue_counts: collections.Counter[str] = collections.Counter()
    issue_counts_by_role: collections.Counter[str] = collections.Counter()
    samples: list[dict[str, Any]] = []
    path_refs_by_id: dict[str, set[str]] = collections.defaultdict(set)
    path_ids_with_tokens: set[str] = set()
    path_event_count = 0

    def record_issue(role: str, issue: str, event: dict[str, Any], detail: dict[str, Any] | None = None) -> None:
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
        if not completed_model_invariant(event):
            continue
        args = event.get("args") if isinstance(event.get("args"), dict) else {}
        for key, value in args.items():
            if "dictionary" not in str(key):
                continue
            dictionary = maybe_json(value)
            if not isinstance(dictionary, dict):
                continue
            path_id = str(dictionary.get("token_path_id") or dictionary.get("path_id") or "")
            tokens = token_id_list(dictionary)
            if path_id and tokens is not None:
                path_ids_with_tokens.add(path_id)

    for _path, event in events:
        if not completed_model_invariant(event):
            continue
        args = event.get("args") if isinstance(event.get("args"), dict) else {}
        role = str(args.get("event_role") or "")
        if role not in roles or role not in PATH_ROLES:
            continue

        path_event_count += 1
        dictionary = maybe_json(args.get("token_dictionary"))
        span = maybe_json(args.get("full_path_span"))
        if maybe_int(args.get("token_count")) is None:
            record_issue(role, "missing_role_token_count", event)

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

            tokens = token_id_list(dictionary)
            if tokens is not None and dictionary_path_id:
                path_ids_with_tokens.add(dictionary_path_id)
                computed_path_id = token_hash(tokens)
                if computed_path_id != dictionary_path_id:
                    record_issue(
                        role,
                        "token_dictionary_hash_mismatch",
                        event,
                        {"path_id": dictionary_path_id, "computed_path_id": computed_path_id},
                    )
                if dictionary_token_count is not None and dictionary_token_count != len(tokens):
                    record_issue(
                        role,
                        "token_dictionary_token_count_mismatch",
                        event,
                        {
                            "path_id": dictionary_path_id,
                            "token_count": dictionary_token_count,
                            "token_ids": len(tokens),
                        },
                    )

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
    """把 run-local request id 映射成 path fact 派生的稳定 fingerprint。

    SGLang request id 每次运行都会重新生成；它在单次 trace 内仍用于关联
    lifecycle/checkpoint fact 和 path-bearing fact，但自身不是跨配置 invariant fact。
    """

    anchors_by_request: dict[str, collections.Counter[str]] = collections.defaultdict(collections.Counter)
    for _path, event in events:
        if not completed_model_invariant(event):
            continue
        args = event.get("args") if isinstance(event.get("args"), dict) else {}
        role = str(args.get("event_role") or "")
        if role not in PATH_ROLES:
            continue
        request_id = str(args.get("request_id") or "")
        if not request_id:
            continue
        anchors_by_request[request_id][request_anchor_signature(role, args)] += 1

    fingerprints: dict[str, str] = {}
    for request_id, anchors in anchors_by_request.items():
        payload = [
            {"count": count, "fact": json.loads(signature)}
            for signature, count in sorted(anchors.items())
        ]
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


def build_signature(role: str, event: dict[str, Any], request_fingerprint: str) -> tuple[str, dict[str, Any]]:
    """为单个 atomic invariant fact 构造 canonical signature。"""

    args = event.get("args") if isinstance(event.get("args"), dict) else {}
    fields: dict[str, Any] = {"role": role}
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
    unknown_invariant_roles: collections.Counter[str] = collections.Counter()
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
        if not completed_model_invariant(event):
            continue
        args = event.get("args") if isinstance(event.get("args"), dict) else {}
        role = str(args.get("event_role") or "")
        if role not in KNOWN_ATOMIC_INVARIANT_ROLES:
            unknown_invariant_roles[role or "missing_event_role"] += 1
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
        signature, fields = build_signature(role, event, request_fingerprint)
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
    return rows, unknown_invariant_roles, unmapped_request_id_events


def summarize_event(event: AuditEvent | None) -> dict[str, Any] | None:
    """把 AuditEvent 转成报告中可读的诊断结构。"""

    if event is None:
        return None
    return {
        "stream": event.stream,
        "ordinal": event.ordinal,
        "role": event.role,
        "target_id": event.target_id,
        "event_name": event.event_name,
        "ts": event.ts,
        "seq_no": event.seq_no,
        "request_id": event.request_id,
        "request_fingerprint": event.request_fingerprint,
        "cache_scope": event.cache_scope,
        "fields": event.fields,
    }


def first_sequence_mismatch(source: list[AuditEvent], target: list[AuditEvent]) -> dict[str, Any] | None:
    """定位 source/target 序列中第一个 signature 不一致位置。"""

    limit = min(len(source), len(target))
    for index in range(limit):
        if source[index].signature != target[index].signature:
            return {
                "index": index,
                "source": summarize_event(source[index]),
                "target": summarize_event(target[index]),
            }
    if len(source) != len(target):
        return {
            "index": limit,
            "source": summarize_event(source[limit] if limit < len(source) else None),
            "target": summarize_event(target[limit] if limit < len(target) else None),
        }
    return None


def counter_samples(counter: collections.Counter[str], sample: int) -> list[dict[str, Any]]:
    """从 signature counter 中提取报告样本。"""

    rows = []
    for signature, count in counter.most_common(sample):
        try:
            fields = json.loads(signature)
        except json.JSONDecodeError:
            fields = {"signature": signature}
        rows.append({"count": count, "fields": fields})
    return rows


def summarize_role(role: str, source: list[AuditEvent], target: list[AuditEvent], sample: int) -> dict[str, Any]:
    """汇总单个 role 的 count、sequence 和 canonical multiset 匹配结果。"""

    source_counter = collections.Counter(event.signature for event in source)
    target_counter = collections.Counter(event.signature for event in target)
    source_only = source_counter - target_counter
    target_only = target_counter - source_counter
    first_mismatch = first_sequence_mismatch(source, target)
    return {
        "role": role,
        "source_count": len(source),
        "target_count": len(target),
        "count_match": len(source) == len(target),
        "sequence_match": first_mismatch is None,
        "signature_multiset_match": not source_only and not target_only,
        "first_sequence_mismatch": first_mismatch,
        "source_only_signature_count": sum(source_only.values()),
        "target_only_signature_count": sum(target_only.values()),
        "source_only_samples": counter_samples(source_only, sample),
        "target_only_samples": counter_samples(target_only, sample),
    }


def build_report(args: argparse.Namespace) -> dict[str, Any]:
    """构造 cross-input audit 完整报告。"""

    roles = set(args.role or DEFAULT_ROLES)
    unknown_roles = sorted(roles - KNOWN_ATOMIC_INVARIANT_ROLES)
    source_events, source_unknown, source_unmapped = extract_audit_events(args.source_trace, args.source_label, roles)
    target_events, target_unknown, target_unmapped = extract_audit_events(args.target_trace, args.target_label, roles)
    source_path_contract = model_input_path_contract(args.source_trace, roles, args.sample)
    target_path_contract = model_input_path_contract(args.target_trace, roles, args.sample)

    source_by_role: dict[str, list[AuditEvent]] = {role: [] for role in roles}
    target_by_role: dict[str, list[AuditEvent]] = {role: [] for role in roles}
    for event in source_events:
        source_by_role.setdefault(event.role, []).append(event)
    for event in target_events:
        target_by_role.setdefault(event.role, []).append(event)

    role_summaries = [
        summarize_role(role, source_by_role.get(role, []), target_by_role.get(role, []), args.sample)
        for role in sorted(roles)
    ]
    fact_mismatch_roles = [
        row["role"]
        for row in role_summaries
        if not row["count_match"] or not row["signature_multiset_match"]
    ]
    sequence_mismatch_roles = [
        row["role"]
        for row in role_summaries
        if row["count_match"] and row["signature_multiset_match"] and not row["sequence_match"]
    ]
    unknown_invariant_roles = {
        "source": dict(sorted(source_unknown.items())),
        "target": dict(sorted(target_unknown.items())),
    }
    unmapped_request_id_events = {
        "source": dict(sorted(source_unmapped.items())),
        "target": dict(sorted(target_unmapped.items())),
    }
    unmapped_roles = sorted(set(source_unmapped) | set(target_unmapped))
    path_contract_roles = sorted(
        set(source_path_contract["blocking_roles"]) | set(target_path_contract["blocking_roles"])
    )
    blocking_roles = sorted(set(fact_mismatch_roles) | set(unmapped_roles) | set(path_contract_roles))
    ready = (
        not blocking_roles
        and not source_unknown
        and not target_unknown
        and not unknown_roles
        and source_path_contract["ready"]
        and target_path_contract["ready"]
    )
    return {
        "schema": "trace_sim.hicache.atomic_cross_input_audit.v3",
        "source_label": args.source_label,
        "target_label": args.target_label,
        "source_traces": [str(path) for path in args.source_trace],
        "target_traces": [str(path) for path in args.target_trace],
        "roles": sorted(roles),
        "source_event_count": len(source_events),
        "target_event_count": len(target_events),
        "model_input_contract_ready": ready,
        "input_contract_ready_for_cross_state_rule_diagnosis": ready,
        "model_input_blocking_roles": blocking_roles,
        "model_input_fact_mismatch_roles": fact_mismatch_roles,
        "model_input_path_contract_blocking_roles": path_contract_roles,
        "source_model_input_path_contract": source_path_contract,
        "target_model_input_path_contract": target_path_contract,
        "non_blocking_sequence_mismatch_roles": sequence_mismatch_roles,
        "unknown_requested_roles": unknown_roles,
        "unknown_invariant_roles": unknown_invariant_roles,
        "unmapped_request_id_events": unmapped_request_id_events,
        "role_summaries": role_summaries,
        "request_id_policy": (
            "Raw request_id is run-local and is not part of the cross-config canonical fact. "
            "Request-scoped facts are compared with a request_fingerprint derived from path-bearing "
            "atomic invariant facts in the same run."
        ),
        "pass_condition": (
            "For every selected atomic invariant role, source and target streams must match by event count "
            "and canonical fact multiset after request_id normalization. Unknown invariant roles and "
            "unmapped request-scoped facts are hard failures. Path-bearing invariant facts must be directly "
            "consumable by the C++ token parser, including at least one model-input token_ids dictionary for "
            "every referenced path_id. Sequence mismatch is diagnostic only."
        ),
    }


def main(argv: list[str] | None = None) -> int:
    """CLI 入口。"""

    args = parse_args(argv)
    report = build_report(args)
    if args.output:
        write_json(args.output, report)
        summary = {
            "schema": report["schema"],
            "output": str(args.output),
            "source_label": report["source_label"],
            "target_label": report["target_label"],
            "source_event_count": report["source_event_count"],
            "target_event_count": report["target_event_count"],
            "model_input_contract_ready": report["model_input_contract_ready"],
            "model_input_blocking_roles": report["model_input_blocking_roles"],
            "model_input_path_contract_blocking_roles": report["model_input_path_contract_blocking_roles"],
            "non_blocking_sequence_mismatch_roles": report["non_blocking_sequence_mismatch_roles"],
            "unknown_invariant_roles": report["unknown_invariant_roles"],
            "unmapped_request_id_events": report["unmapped_request_id_events"],
        }
        print(json.dumps(summary, ensure_ascii=False, indent=2))
    else:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
