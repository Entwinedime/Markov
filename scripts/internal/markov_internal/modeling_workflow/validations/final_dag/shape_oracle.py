"""Independent target-actual shape oracle for HiCache DAG patch validation."""

from __future__ import annotations

from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable

from ....common.trace import load_chrome_trace_events
from ..hicache.core.facts import HICACHE_CONSUMER_DAG_PATCH, parse_fact_or_none
from .shape_compare import ACTIVE_STATES as _ACTIVE_STATES
from .shape_compare import actual_row as _actual_row
from .shape_facts import (
    cache_extend_consumer as _cache_extend_consumer,
    candidate_transfer_token_count as _candidate_transfer_token_count,
    commit_release_calls as _commit_release_calls,
    commit_request_events as _commit_request_events,
    completed_state_fact_phase as _completed_state_fact_phase,
    completed_transfer_tokens as _completed_transfer_tokens,
    event_sort_key as _event_sort_key,
    index_roles as _index_roles,
    nested_events as _nested_events,
    observed_transfer_tokens as _observed_transfer_tokens,
    opportunity_identities as _opportunity_identities,
    optional_bool as _optional_bool,
    optional_u64 as _optional_u64,
    prefetch_operation_window_end as _prefetch_operation_window_end,
    request_events as _request_events,
    request_window_end as _request_window_end,
    timing_calls_for_node as _timing_calls_for_node,
    timing_calls_for_nodes as _timing_calls_for_nodes,
    transfer_state as _transfer_state,
    u64 as _u64,
    u64_list as _u64_list,
)


_EFFECT_DESCRIPTORS = {
    "cache_lookup_input": (
        ("loadback", "host_to_device", "host_to_device_lane"),
        ("commit_device_to_host", "device_to_host", "device_to_host_lane"),
        ("commit_host_to_storage", "host_to_storage", "host_storage_lane"),
        ("commit_capacity_gate", "none", ""),
    ),
    "cache_extend_input": (
        ("commit_device_to_host", "device_to_host", "device_to_host_lane"),
        ("commit_host_to_storage", "host_to_storage", "host_storage_lane"),
        ("commit_capacity_gate", "none", ""),
    ),
    "prefetch_candidate_anchor": (
        ("prefetch_io_operation", "storage_to_host", "host_storage_lane"),
        ("prefetch_visibility_dependency", "none", ""),
    ),
    "cache_lifecycle_commit": (
        ("commit_device_to_host", "device_to_host", "device_to_host_lane"),
        ("commit_host_to_storage", "host_to_storage", "host_storage_lane"),
        ("commit_capacity_gate", "none", ""),
    ),
}
def extract_target_shape_oracle(
    trace_paths: Iterable[Path],
    *,
    patch_probe_contract_ready: bool,
    window_start_us: int | None = None,
    window_end_us: int | None = None,
) -> dict[str, Any]:
    """Extract target effect shape without reading model or predicted-patch artifacts."""

    events, parse_errors = _load_fact_events(
        trace_paths,
        window_start_us=window_start_us,
        window_end_us=window_end_us,
    )
    opportunities, opportunity_blockers = _build_opportunities(events)
    role_events = _index_roles(events)
    rows = [
        _classify_opportunity(
            opportunity,
            opportunities,
            role_events,
            patch_probe_contract_ready=patch_probe_contract_ready,
        )
        for opportunity in opportunities
    ]
    _annotate_family_relations(rows)
    lane_orders = _annotate_lane_orders(rows)

    blockers = Counter(opportunity_blockers)
    blockers.update(parse_errors)
    if not patch_probe_contract_ready:
        blockers["patch_probe_contract_not_enabled"] += 1
    for row in rows:
        if row["actual_state"] in {"unobservable", "ambiguous"}:
            blocker = str(row.get("blocker") or f"{row['effect_type']}_not_observable")
            if blocker != "patch_probe_contract_not_enabled":
                blockers[blocker] += 1

    status = "ready" if rows and not blockers else "not_ready"
    return {
        "ready": status == "ready",
        "lane_orders": lane_orders,
        "effects": rows,
    }


def patch_probe_contract_enabled(profile_manifest: dict[str, Any]) -> bool:
    """Return whether profiling explicitly enabled the DAG-patch evidence contract."""

    profiling = profile_manifest.get("profiling") if isinstance(profile_manifest.get("profiling"), dict) else {}
    consumers = profiling.get("python_consumers") if isinstance(profiling.get("python_consumers"), list) else []
    return HICACHE_CONSUMER_DAG_PATCH in consumers


def _load_fact_events(
    trace_paths: Iterable[Path],
    *,
    window_start_us: int | None,
    window_end_us: int | None,
) -> tuple[list[dict[str, Any]], Counter[str]]:
    events: list[dict[str, Any]] = []
    errors: Counter[str] = Counter()
    for file_index, path in enumerate(trace_paths):
        rows, status = load_chrome_trace_events(path, auto_repair=True)
        if not status.loaded:
            errors["target_trace_load_failed"] += 1
            continue
        for ordinal, event in enumerate(rows):
            args = event.get("args") if isinstance(event.get("args"), dict) else {}
            try:
                fact = parse_fact_or_none(args)
            except ValueError:
                errors["malformed_hicache_fact"] += 1
                continue
            if fact is None:
                continue
            timestamp = _u64(event.get("ts"))
            duration = _u64(event.get("dur"))
            event_end = timestamp + duration
            window_position = "formal"
            if window_start_us is not None and event_end < window_start_us:
                continue
            if window_end_us is not None and timestamp > window_end_us:
                window_position = "post"
            events.append(
                {
                    "fact_class": fact.fact_class,
                    "role": fact.role,
                    "args": args,
                    "name": str(event.get("name") or ""),
                    "pid": str(event.get("pid") or ""),
                    "tid": str(event.get("tid") or ""),
                    "ts": timestamp,
                    "dur": duration,
                    "end": event_end,
                    "window_position": window_position,
                    "file_index": file_index,
                    "ordinal": ordinal,
                }
            )
    events.sort(key=_event_sort_key)
    return events, errors


def _build_opportunities(
    events: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], Counter[str]]:
    opportunities: list[dict[str, Any]] = []
    blockers: Counter[str] = Counter()
    scope_ordinals: dict[str, str] = {}
    role_ordinals: Counter[tuple[str, str]] = Counter()
    for event in events:
        window_position = str(event.get("window_position") or "")
        if window_position != "formal":
            continue
        role = str(event["role"])
        descriptors = _EFFECT_DESCRIPTORS.get(role)
        if not descriptors or event["fact_class"] != "workload_identity" or not _completed_state_fact_phase(event):
            continue
        args = event["args"]
        raw_scope = str(args.get("cache_scope") or "")
        identities = _opportunity_identities(event)
        if not identities:
            blockers["opportunity_identity_incomplete"] += len(descriptors)
            continue
        for request_id, span in identities:
            path_id = str(span.get("path_id") or span.get("token_path_id") or "")
            begin = _u64(span.get("begin"))
            end = _u64(span.get("end"))
            if not raw_scope or not path_id or end < begin:
                blockers["opportunity_identity_incomplete"] += len(descriptors)
                continue
            scope = scope_ordinals.setdefault(raw_scope, f"scope:{len(scope_ordinals) + 1}")
            role_ordinals[(scope, role)] += 1
            role_ordinal = role_ordinals[(scope, role)]
            request_identity = f"{path_id}:{begin}:{end}"
            family_key = f"{scope}|{request_identity}|{role}|{role_ordinal}"
            for decision_ordinal, (effect_type, direction, lane) in enumerate(descriptors):
                opportunity = {
                    "effect_key": f"{effect_type}|{family_key}|{decision_ordinal}",
                    "effect_family_key": family_key,
                    "effect_type": effect_type,
                    "direction": direction,
                    "resource_lane": lane,
                    "request_id": request_id,
                    "source_fact_role": role,
                    "cache_scope": scope,
                    "raw_scope": raw_scope,
                    "pid": event["pid"],
                    "tid": event["tid"],
                    "candidate_token_count": end - begin,
                    "anchor": event,
                }
                opportunities.append(opportunity)
    keys = [str(item["effect_key"]) for item in opportunities]
    blockers["duplicate_stable_effect_key"] += len(keys) - len(set(keys))
    if blockers["duplicate_stable_effect_key"] == 0:
        del blockers["duplicate_stable_effect_key"]
    return opportunities, blockers


def _classify_opportunity(
    opportunity: dict[str, Any],
    opportunities: list[dict[str, Any]],
    role_events: dict[str, list[dict[str, Any]]],
    *,
    patch_probe_contract_ready: bool,
) -> dict[str, Any]:
    if not patch_probe_contract_ready:
        return _actual_row(opportunity, "unobservable", blocker="patch_probe_contract_not_enabled")
    effect_type = str(opportunity["effect_type"])
    if effect_type == "loadback":
        return _classify_loadback(opportunity, opportunities, role_events)
    if effect_type == "prefetch_io_operation":
        return _classify_prefetch_io(opportunity, opportunities, role_events)
    if effect_type == "prefetch_visibility_dependency":
        return _classify_prefetch_visibility(opportunity, opportunities, role_events)
    if effect_type == "commit_device_to_host":
        return _classify_commit_d2h(opportunity, role_events)
    if effect_type == "commit_host_to_storage":
        return _classify_commit_h2s(opportunity, role_events)
    if effect_type == "commit_capacity_gate":
        return _classify_commit_capacity(opportunity, role_events)
    return _actual_row(opportunity, "unobservable", blocker="unsupported_effect_type")


def _classify_loadback(
    opportunity: dict[str, Any], opportunities: list[dict[str, Any]], role_events: dict[str, list[dict[str, Any]]]
) -> dict[str, Any]:
    window_end = _request_window_end(opportunity, opportunities, role_events)
    decisions = _request_events(opportunity, role_events.get("loadback_decision_observed", []), window_end)
    if not decisions:
        return _actual_row(opportunity, "not_required")
    if len(decisions) != 1:
        return _actual_row(opportunity, "ambiguous", blocker="multiple_loadback_decisions")
    decision = decisions[0]
    effective = _u64(decision["args"].get("effective_token_count"))
    if effective == 0:
        return _actual_row(opportunity, "not_required")
    node_id = _optional_u64(decision["args"].get("node_id"))
    timings = _timing_calls_for_node(
        role_events.get("loadback_io_observed", []),
        node_id,
        opportunity["pid"],
        lower_bound_us=int(decision["ts"]),
        upper_bound_us=window_end,
    )
    if len(timings) != 1:
        state = "ambiguous" if len(timings) > 1 else "unobservable"
        return _actual_row(opportunity, state, blocker="loadback_timing_identity_incomplete")
    state = _transfer_state(effective, _candidate_transfer_token_count(opportunity))
    return _actual_row(
        opportunity,
        state,
        operation_sort_key=_event_sort_key(timings[0]),
    )


def _classify_prefetch_io(
    opportunity: dict[str, Any], opportunities: list[dict[str, Any]], role_events: dict[str, list[dict[str, Any]]]
) -> dict[str, Any]:
    window_end = _prefetch_operation_window_end(opportunity, opportunities)
    timings = _request_events(opportunity, role_events.get("prefetch_io_observed", []), window_end)
    timings = [event for event in timings if event["dur"] > 0]
    if len(timings) > 1:
        return _actual_row(opportunity, "ambiguous", blocker="multiple_prefetch_io_operations")
    if not timings:
        return _actual_row(opportunity, "not_required")
    timing = timings[0]
    completed = _completed_transfer_tokens(timing["args"])
    state = _transfer_state(completed, _candidate_transfer_token_count(opportunity))
    return _actual_row(
        opportunity,
        state,
        operation_sort_key=_event_sort_key(timing),
    )


def _classify_prefetch_visibility(
    opportunity: dict[str, Any], opportunities: list[dict[str, Any]], role_events: dict[str, list[dict[str, Any]]]
) -> dict[str, Any]:
    window_end = _request_window_end(opportunity, opportunities, role_events)
    progress = _request_events(opportunity, role_events.get("prefetch_progress_observed", []), window_end)
    intents = _request_events(opportunity, role_events.get("prefetch_intent_observed", []), window_end)
    transfers = _request_events(opportunity, role_events.get("prefetch_io_observed", []), window_end)
    transfers = [event for event in transfers if event["dur"] > 0]
    if len(transfers) > 1:
        return _actual_row(opportunity, "ambiguous", blocker="multiple_prefetch_visibility_transfers")
    if not transfers:
        return _actual_row(opportunity, "not_required")
    if not intents or not progress:
        return _actual_row(opportunity, "unobservable", blocker="prefetch_visibility_progress_missing")
    progress_results = [_optional_bool(event["args"].get("progress_ready")) for event in progress]
    if any(result is None for result in progress_results):
        return _actual_row(opportunity, "unobservable", blocker="prefetch_progress_result_missing")
    if all(progress_results):
        return _actual_row(opportunity, "not_required")
    consumer = _cache_extend_consumer(opportunity, role_events)
    if consumer is None:
        return _actual_row(opportunity, "unobservable", blocker="prefetch_visibility_consumer_missing")
    return _actual_row(
        opportunity,
        "required",
        actual_consumer_role="cache_extend_input",
    )


def _classify_commit_d2h(opportunity: dict[str, Any], role_events: dict[str, list[dict[str, Any]]]) -> dict[str, Any]:
    enqueues = _commit_request_events(
        opportunity,
        role_events,
        role_events.get("commit_device_to_host_enqueue_observed", []),
    )
    enqueues = [
        event
        for event in enqueues
        if str(event["args"].get("phase") or "") == "end" and event["window_position"] == "formal"
    ]
    effective = sum(_u64(event["args"].get("effective_token_count")) for event in enqueues)
    if not enqueues or effective == 0:
        return _actual_row(opportunity, "not_required")
    node_ids = {node_id for event in enqueues for node_id in _u64_list(event["args"].get("node_id"))}
    timings = _timing_calls_for_nodes(
        role_events.get("commit_device_to_host_io_observed", []), node_ids, opportunity["pid"]
    )
    if not timings:
        return _actual_row(opportunity, "unobservable", blocker="commit_d2h_timing_identity_incomplete")
    state = _transfer_state(effective, effective)
    return _actual_row(
        opportunity,
        state,
        operation_sort_key=min((_event_sort_key(event) for event in timings)),
    )


def _classify_commit_h2s(opportunity: dict[str, Any], role_events: dict[str, list[dict[str, Any]]]) -> dict[str, Any]:
    releases = _commit_release_calls(opportunity, role_events)
    enqueues = [
        event
        for release in releases
        for event in _nested_events(release, role_events.get("writeback_enqueue_observed", []))
        if str(event["args"].get("phase") or "") == "end"
    ]
    if not enqueues:
        return _actual_row(opportunity, "not_required")
    candidate = sum(_observed_transfer_tokens(event["args"]) for event in enqueues)
    if candidate == 0:
        return _actual_row(opportunity, "unobservable", blocker="commit_h2s_payload_identity_incomplete")
    timings: list[dict[str, Any]] = []
    for enqueue in enqueues:
        operation_id = str(enqueue["args"].get("operation_id") or "")
        matches = [
            event
            for event in role_events.get("writeback_io_observed", [])
            if event["pid"] == opportunity["pid"]
            and str(event["args"].get("operation_id") or "") == operation_id
            and event["dur"] > 0
        ]
        if len(matches) != 1:
            return _actual_row(
                opportunity,
                "ambiguous" if len(matches) > 1 else "unobservable",
                blocker="commit_h2s_operation_join_incomplete",
            )
        timings.extend(matches)
    completed = sum(_completed_transfer_tokens(event["args"]) for event in timings)
    state = _transfer_state(completed, candidate)
    return _actual_row(
        opportunity,
        state,
        operation_sort_key=min((_event_sort_key(event) for event in timings)),
    )


def _classify_commit_capacity(
    opportunity: dict[str, Any], role_events: dict[str, list[dict[str, Any]]]
) -> dict[str, Any]:
    d2h = _commit_request_events(
        opportunity,
        role_events,
        role_events.get("commit_device_to_host_enqueue_observed", []),
    )
    d2h = [event for event in d2h if _u64(event["args"].get("effective_token_count")) > 0]
    if not d2h:
        return _actual_row(opportunity, "not_required")
    releases = _commit_release_calls(opportunity, role_events)
    if not releases:
        return _actual_row(opportunity, "unobservable", blocker="commit_capacity_release_missing")
    return _actual_row(opportunity, "not_required")


def _annotate_family_relations(rows: list[dict[str, Any]]) -> None:
    families: dict[str, dict[str, dict[str, Any]]] = defaultdict(dict)
    for row in rows:
        families[str(row["effect_family_key"])][str(row["effect_type"])] = row
    for family in families.values():
        for effect_type, row in family.items():
            active = row["actual_state"] in _ACTIVE_STATES
            if not active:
                row["consumer_role"] = "none"
                row["blocking_relation"] = "none"
            elif effect_type == "loadback":
                row["consumer_role"] = "foreground_cache_consumer"
                row["blocking_relation"] = "blocking"
            elif effect_type == "prefetch_io_operation":
                visibility = family.get("prefetch_visibility_dependency")
                row["consumer_role"] = (
                    "prefetch_visibility_dependency"
                    if visibility and visibility["actual_state"] in _ACTIVE_STATES
                    else "background_completion"
                )
                row["blocking_relation"] = "background"
            elif effect_type == "prefetch_visibility_dependency":
                row["consumer_role"] = str(row.pop("actual_consumer_role", "cache_extend_input"))
                row["blocking_relation"] = "blocking"
            elif effect_type == "commit_device_to_host":
                if family.get("commit_host_to_storage", {}).get("actual_state") in _ACTIVE_STATES:
                    row["consumer_role"] = "commit_host_to_storage"
                elif family.get("commit_capacity_gate", {}).get("actual_state") in _ACTIVE_STATES:
                    row["consumer_role"] = "commit_capacity_gate"
                else:
                    row["consumer_role"] = "commit_completion"
                row["blocking_relation"] = "family_internal"
            elif effect_type == "commit_host_to_storage":
                row["consumer_role"] = (
                    "commit_capacity_gate"
                    if family.get("commit_capacity_gate", {}).get("actual_state") in _ACTIVE_STATES
                    else "storage_completion"
                )
                row["blocking_relation"] = "family_internal"
            elif effect_type == "commit_capacity_gate":
                row["consumer_role"] = str(row.pop("actual_consumer_role", "next_capacity_consumer"))
                row["blocking_relation"] = "blocking"
            row.pop("actual_consumer_role", None)


def _annotate_lane_orders(rows: list[dict[str, Any]]) -> dict[str, list[str]]:
    by_lane: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        if row["actual_state"] in _ACTIVE_STATES and row["resource_lane"] and row.get("operation_sort_key") is not None:
            by_lane[str(row["resource_lane"])].append(row)
    orders: dict[str, list[str]] = {}
    for lane, lane_rows in sorted(by_lane.items()):
        lane_rows.sort(key=lambda row: (row["operation_sort_key"], row["effect_key"]))
        orders[lane] = [str(row["effect_key"]) for row in lane_rows]
        for ordinal, row in enumerate(lane_rows):
            row["lane_order_ordinal"] = ordinal
    for row in rows:
        row.pop("operation_sort_key", None)
    return orders
