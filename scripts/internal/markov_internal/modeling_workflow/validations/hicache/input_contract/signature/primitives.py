"""workload signature 的无状态解析与 canonical 辅助工具。"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from markov_internal.common.trace import load_chrome_trace_events
from ...core.facts import HICACHE_CONSUMER_INPUT_CONTRACT, parse_fact_or_none
from ...core.tokens import token_id_path, token_path_count, token_path_hash
from .types import ROLE_SCALAR_FIELDS


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


def fact_items(value: Any) -> list[Any]:
    """把 scalar fact 字段和数组 fact 字段统一成列表。"""

    if isinstance(value, list):
        return value
    if value is None:
        return []
    return [value]


def scalar_value(args: dict[str, Any], field: str) -> Any:
    """读取 signature 中允许参与比较的标量或结构化字段。"""

    value = maybe_json(args.get(field))
    if isinstance(value, (dict, list)):
        return value
    if value is None:
        return None
    return str(value)


def request_anchor_signature_fields(role: str, args: dict[str, Any]) -> dict[str, Any]:
    """构造 request fingerprint anchor 的 canonical 字段。"""

    fields: dict[str, Any] = {"role": role}
    for field in ROLE_SCALAR_FIELDS.get(role, ()):
        value = scalar_value(args, field)
        if value is not None:
            fields[field] = value
    fields["path"] = path_signature(args)
    return fields
