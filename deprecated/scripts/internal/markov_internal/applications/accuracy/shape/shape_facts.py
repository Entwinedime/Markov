"""Trace-fact joins used by the target-observed HiCache shape oracle."""

from __future__ import annotations

import json
from bisect import bisect_left
from collections import Counter, defaultdict
from typing import Any


_OPPORTUNITY_ROLES = (
    "cache_lookup_input",
    "cache_extend_input",
    "cache_decode_allocation",
    "prefetch_candidate_anchor",
    "cache_lifecycle_commit",
)


def index_roles(events: list[dict[str, Any]]) -> dict[str, list[dict[str, Any]]]:
    result: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for event in events:
        result[str(event["role"])].append(event)
    return result


def derive_decode_opportunities(events: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], Counter[str]]:
    """Join observed Decode allocation calls to prompt identity and their actual forward.

    Only successful single-token calls with consecutive, observed request lengths
    establish an opportunity. Missing joins remain report blockers, not zero work.
    This reads target observations only; no predicted decisions or costs are used.
    """

    forwards: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for event in events:
        if event["role"] == "decode_forward_observed" and event["window_position"] == "formal":
            forwards[(event["pid"], event["tid"])].append(event)
    times = {lane: [event["ts"] for event in rows] for lane, rows in forwards.items()}
    used: set[tuple[tuple[str, str], int]] = set()
    origins: dict[tuple[str, str], tuple[dict[str, Any], dict[str, Any]]] = {}
    iterations: Counter[tuple[str, str]] = Counter()
    blockers: Counter[str] = Counter()
    derived = []

    for event in events:
        if event["window_position"] != "formal":
            continue
        if event["role"] == "cache_extend_input" and completed_state_fact_phase(event):
            for request_id, span in opportunity_identities(event):
                key = (event["pid"], request_id)
                if key in origins:
                    blockers["decode_ambiguous_prompt_origin"] += 1
                origins[key] = (event, span)
            continue
        if event["role"] != "decode_allocation_observed":
            continue

        args = event["args"]
        requests = array_arg(args.get("requests"))
        ids = [str(request["request_id"]) for request in requests]
        lane = (event["pid"], event["tid"])
        position = bisect_left(times.get(lane, []), event["end"])
        forward = forwards[lane][position] if position < len(forwards[lane]) else None
        if (
            args.get("status") != "returned"
            or args.get("token_per_req") != 1
            or not requests
            or forward is None
            or forward["args"].get("status") != "returned"
            or array_arg(forward["args"].get("request_ids")) != ids
            or (lane, position) in used
        ):
            blockers["decode_allocation_forward_join_incomplete"] += 1
            continue

        spans = []
        for request_id, request in zip(ids, requests):
            key = (event["pid"], request_id)
            origin = origins.get(key)
            if origin is None:
                break
            parent, span = origin
            length = u64(span.get("end")) - u64(span.get("begin")) + iterations[key]
            if (
                parent["tid"] != event["tid"]
                or parent["args"].get("cache_scope") != args.get("cache_scope")
                or parent["ts"] >= event["ts"]
                or request.get("decode_batch_idx") != iterations[key]
                or request.get("kv_committed_len") != length
                or request.get("kv_allocated_len") != length
            ):
                break
            spans.append(span)
        if len(spans) != len(requests):
            blockers["decode_request_identity_incomplete"] += 1
            continue

        used.add((lane, position))
        for request_id in ids:
            iterations[(event["pid"], request_id)] += 1
        derived.append(
            dict(
                event,
                role="cache_decode_allocation",
                fact_class="workload_identity",
                args=dict(
                    args, phase="end", request_ids=ids, full_path_spans=spans, source_page_size=args["page_size_tokens"]
                ),
            )
        )

    missing = sum(len(rows) for rows in forwards.values()) - len(used)
    if missing:
        blockers["decode_forward_without_allocation"] += missing
    return sorted([*events, *derived], key=event_sort_key), blockers


def request_events(opportunity: dict[str, Any], events: list[dict[str, Any]], window_end: int) -> list[dict[str, Any]]:
    request_id = str(opportunity["request_id"])
    start = int(opportunity["anchor"]["ts"])
    return [
        event
        for event in events
        if event["pid"] == opportunity["pid"]
        and str(event["args"].get("request_id") or "") == request_id
        and start <= event["ts"] <= window_end
    ]


def request_window_end(
    opportunity: dict[str, Any],
    opportunities: list[dict[str, Any]],
    role_events: dict[str, list[dict[str, Any]]],
) -> int:
    consumer = cache_extend_consumer(opportunity, role_events)
    candidates = [consumer["ts"]] if consumer is not None else []
    start = int(opportunity["anchor"]["ts"])
    candidates.extend(
        int(item["anchor"]["ts"])
        for item in opportunities
        if item["pid"] == opportunity["pid"]
        and item["request_id"] == opportunity["request_id"]
        and item["source_fact_role"] == opportunity["source_fact_role"]
        and int(item["anchor"]["ts"]) > start
    )
    return min(candidates) if candidates else 2**63 - 1


def prefetch_operation_window_end(opportunity: dict[str, Any], opportunities: list[dict[str, Any]]) -> int:
    """Keep payload observation open until a repeated candidate."""

    start = int(opportunity["anchor"]["ts"])
    candidates = [
        int(item["anchor"]["ts"])
        for item in opportunities
        if item["pid"] == opportunity["pid"]
        and item["request_id"] == opportunity["request_id"]
        and item["source_fact_role"] == opportunity["source_fact_role"]
        and int(item["anchor"]["ts"]) > start
    ]
    return min(candidates) if candidates else 2**63 - 1


def cache_extend_consumer(
    opportunity: dict[str, Any], role_events: dict[str, list[dict[str, Any]]]
) -> dict[str, Any] | None:
    request_id = str(opportunity["request_id"])
    start = int(opportunity["anchor"]["ts"])
    candidates = [
        event
        for event in role_events.get("cache_extend_input", [])
        if event["pid"] == opportunity["pid"] and event["ts"] >= start and request_id in request_ids(event["args"])
    ]
    return min(candidates, key=event_sort_key) if candidates else None


def next_canonical_consumer(
    opportunity: dict[str, Any], role_events: dict[str, list[dict[str, Any]]]
) -> dict[str, Any] | None:
    anchor_key = canonical_boundary_sort_key(opportunity["anchor"])
    candidates = [
        event
        for role in _OPPORTUNITY_ROLES
        for event in role_events.get(role, [])
        if event["fact_class"] == "workload_identity"
        and completed_state_fact_phase(event)
        and event["pid"] == opportunity["pid"]
        and str(event["args"].get("cache_scope") or "") == opportunity["raw_scope"]
        and canonical_boundary_sort_key(event) > anchor_key
    ]
    return min(candidates, key=canonical_boundary_sort_key) if candidates else None


def nested_events(parent: dict[str, Any], events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [event for event in events if nested(event, parent)]


def nested(child: dict[str, Any], parent: dict[str, Any]) -> bool:
    return (
        child["pid"] == parent["pid"]
        and child["tid"] == parent["tid"]
        and child["ts"] >= parent["ts"]
        and child["end"] <= parent["end"]
    )


def commit_release_calls(
    opportunity: dict[str, Any], role_events: dict[str, list[dict[str, Any]]]
) -> list[dict[str, Any]]:
    enqueues = commit_request_events(
        opportunity,
        role_events,
        role_events.get("commit_device_to_host_enqueue_observed", []),
    )
    node_ids = {
        node_id
        for event in enqueues
        if u64(event["args"].get("effective_token_count")) > 0
        for node_id in u64_list(event["args"].get("node_id"))
    }
    starts = [
        event
        for event in role_events.get("commit_capacity_release_observed", [])
        if event["pid"] == opportunity["pid"]
        and str(event["args"].get("phase") or "") == "start"
        and node_ids.intersection(u64_list(event["args"].get("operation_node_ids")))
    ]
    return [
        # The acknowledgement removes its pending entry before the end probe.
        # Keep node ownership observed at entry with the completed call.
        dict(end, args={**end["args"], "operation_node_ids": start["args"]["operation_node_ids"]})
        for start in starts
        if (end := paired_end(start, role_events.get("commit_capacity_release_observed", []))) is not None
    ]


def commit_request_events(
    opportunity: dict[str, Any],
    role_events: dict[str, list[dict[str, Any]]],
    events: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """Return operation events owned by the state fact that caused the commit."""

    anchor = opportunity["anchor"]
    if opportunity["source_fact_role"] == "cache_lookup_input":
        consumer = cache_extend_consumer(opportunity, role_events)
        window_start = int(anchor["end"])
        request_id = str(opportunity.get("request_id") or "")
        later_lookups = [
            event
            for event in role_events.get("cache_lookup_input", [])
            if event["pid"] == opportunity["pid"]
            and str(event["args"].get("cache_scope") or "") == opportunity["raw_scope"]
            and str(event["args"].get("request_id") or "") == request_id
            and int(event["ts"]) > int(anchor["ts"])
        ]
        boundaries = [int(consumer["ts"])] if consumer is not None else []
        boundaries.extend(int(event["ts"]) for event in later_lookups)
        window_end = min(boundaries) if boundaries else 2**63 - 1
        return [
            event
            for event in events
            if event["pid"] == opportunity["pid"]
            and event["tid"] == opportunity["tid"]
            and window_start <= int(event["ts"])
            and int(event["end"]) <= window_end
        ]
    if str(anchor["args"].get("phase") or "") == "start":
        completed = paired_end(anchor, role_events.get(str(opportunity["source_fact_role"]), []))
        if completed is not None:
            anchor = completed
        else:
            consumer = next_canonical_consumer(opportunity, role_events)
            window_end = int(consumer["ts"]) if consumer is not None else 2**63 - 1
            return [
                event
                for event in events
                if event["pid"] == opportunity["pid"]
                and event["tid"] == opportunity["tid"]
                and int(anchor["ts"]) <= int(event["ts"])
                and int(event["end"]) <= window_end
            ]
    return nested_events(anchor, events)


def timing_calls_for_node(
    events: list[dict[str, Any]],
    node_id: int | None,
    pid: str,
    *,
    lower_bound_us: int | None = None,
    upper_bound_us: int | None = None,
) -> list[dict[str, Any]]:
    return timing_calls_for_nodes(
        events,
        set() if node_id is None else {node_id},
        pid,
        lower_bound_us=lower_bound_us,
        upper_bound_us=upper_bound_us,
    )


def timing_calls_for_nodes(
    events: list[dict[str, Any]],
    node_ids: set[int],
    pid: str,
    *,
    lower_bound_us: int | None = None,
    upper_bound_us: int | None = None,
) -> list[dict[str, Any]]:
    if not node_ids:
        return []
    starts = [
        event
        for event in events
        if event["pid"] == pid
        and str(event["args"].get("phase") or "") == "start"
        and node_ids.intersection(u64_list(event["args"].get("operation_node_ids")))
        and (lower_bound_us is None or int(event["ts"]) >= lower_bound_us)
    ]
    return [
        end
        for start in starts
        if (end := paired_end(start, events)) is not None
        and end["dur"] > 0
        and (upper_bound_us is None or int(end["end"]) <= upper_bound_us)
    ]


def paired_end(start: dict[str, Any], events: list[dict[str, Any]]) -> dict[str, Any] | None:
    target_id = str(start["args"].get("target_id") or "")
    matches = [
        event
        for event in events
        if str(event["args"].get("phase") or "") == "end"
        and str(event["args"].get("target_id") or "") == target_id
        and event["pid"] == start["pid"]
        and event["tid"] == start["tid"]
        and event["ts"] == start["ts"]
    ]
    return matches[0] if len(matches) == 1 else None


def transfer_state(effective: int, candidate: int) -> str:
    if effective <= 0:
        return "not_required"
    if candidate > 0 and effective < candidate:
        return "partial"
    return "required"


def candidate_transfer_token_count(opportunity: dict[str, Any]) -> int:
    candidate = int(opportunity["candidate_token_count"])
    page_size = u64(opportunity["anchor"]["args"].get("source_page_size"))
    return candidate if page_size == 0 else candidate - candidate % page_size


def completed_transfer_tokens(args: dict[str, Any]) -> int:
    for key in ("completed_token_count", "effective_token_count", "token_count"):
        if key in args:
            return u64(args.get(key))
    return 0


def observed_transfer_tokens(args: dict[str, Any]) -> int:
    completed = completed_transfer_tokens(args)
    if completed > 0:
        return completed
    span = object_arg(args.get("full_path_span"))
    return u64(span.get("token_count")) or max(0, u64(span.get("end")) - u64(span.get("begin")))


def scoped_resource_lane(scope: str, lane: str) -> str:
    return f"{scope}/{lane}" if scope and lane else ""


def completed_state_fact_phase(event: dict[str, Any]) -> bool:
    phase = str(event["args"].get("phase") or "").lower()
    return phase == ("start" if event["role"] == "cache_extend_input" else "end")


def canonical_boundary_sort_key(event: dict[str, Any]) -> tuple[Any, ...]:
    phase = str(event["args"].get("phase") or "").lower()
    timestamp = int(event["end"]) if phase == "end" else int(event["ts"])
    return (timestamp, *event_sort_key(event)[1:])


def event_sort_key(event: dict[str, Any]) -> tuple[Any, ...]:
    return (
        int(event["ts"]),
        str(event["pid"]),
        str(event["tid"]),
        str(event["name"]),
        int(event["file_index"]),
        int(event["ordinal"]),
    )


def request_ids(args: dict[str, Any]) -> set[str]:
    value = args.get("request_ids", args.get("batch_request_ids", []))
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except json.JSONDecodeError:
            return set()
    return {str(item) for item in value} if isinstance(value, list) else set()


def opportunity_identities(event: dict[str, Any]) -> list[tuple[str, dict[str, Any]]]:
    args = event["args"]
    if event["role"] not in {"cache_extend_input", "cache_decode_allocation"}:
        span = object_arg(args.get("full_path_span"))
        return [(str(args.get("request_id") or ""), span)] if span else []
    request_values = array_arg(args.get("request_ids", args.get("batch_request_ids", [])))
    spans = array_arg(args.get("full_path_spans"))
    if len(request_values) != len(spans):
        return []
    return [
        (str(request_id), span) for request_id, raw_span in zip(request_values, spans) if (span := object_arg(raw_span))
    ]


def array_arg(value: Any) -> list[Any]:
    if isinstance(value, list):
        return value
    if isinstance(value, str):
        try:
            parsed = json.loads(value)
        except json.JSONDecodeError:
            return []
        return parsed if isinstance(parsed, list) else []
    return []


def object_arg(value: Any) -> dict[str, Any]:
    if isinstance(value, dict):
        return value
    if isinstance(value, str):
        try:
            parsed = json.loads(value)
        except json.JSONDecodeError:
            return {}
        return parsed if isinstance(parsed, dict) else {}
    return {}


def u64_list(value: Any) -> list[int]:
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except json.JSONDecodeError:
            value = [value]
    if not isinstance(value, list):
        value = [value]
    return [number for item in value if (number := optional_u64(item)) is not None]


def u64(value: Any) -> int:
    number = optional_u64(value)
    return number if number is not None else 0


def optional_u64(value: Any) -> int | None:
    try:
        number = int(value)
    except (TypeError, ValueError):
        return None
    return number if number >= 0 else None


def optional_bool(value: Any) -> bool | None:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)) and value in (0, 1):
        return bool(value)
    if isinstance(value, str):
        normalized = value.strip().lower()
        if normalized in {"true", "1"}:
            return True
        if normalized in {"false", "0"}:
            return False
    return None
