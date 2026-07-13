"""Independent target-actual shape oracle for HiCache DAG patch validation."""

from __future__ import annotations

import json
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable

from ....common.trace import load_chrome_trace_events
from ..hicache.core.facts import HICACHE_CONSUMER_DAG_PATCH, parse_fact_or_none


_EFFECT_DESCRIPTORS = {
    "cache_lookup_input": (("loadback", "host_to_device", "host_to_device_lane"),),
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
_TRANSFER_EFFECTS = {
    "loadback",
    "prefetch_io_operation",
    "commit_device_to_host",
    "commit_host_to_storage",
}
_ACTIVE_STATES = {"required", "partial"}
_ARRIVAL_SCHEDULE_SENSITIVE_EFFECTS = {"loadback", "prefetch_visibility_dependency"}
_PREFETCH_READINESS_LIMITATION = "payload_only_control_pipeline_unmodeled"


def extract_target_shape_oracle(
    trace_paths: Iterable[Path],
    *,
    target_run_id: str,
    target_config_id: str,
    input_id: str,
    patch_probe_contract_ready: bool,
) -> dict[str, Any]:
    """Extract target effect shape without reading model or predicted-patch artifacts."""

    events, load_statuses, parse_errors = _load_fact_events(trace_paths)
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

    state_counts = Counter(str(row["actual_state"]) for row in rows)
    sensitivity_counts = Counter(str(row["schedule_sensitivity"]) for row in rows)
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
        "schema": "trace_sim.hicache.target_effect_shape_oracle.v1",
        "status": status,
        "ready": status == "ready",
        "target_run_id": target_run_id,
        "target_config_id": target_config_id,
        "input_id": input_id,
        "patch_probe_contract_ready": patch_probe_contract_ready,
        "trace_files": [str(path) for path in trace_paths],
        "trace_load_status": load_statuses,
        "opportunity_count": len(rows),
        "counts_by_actual_state": dict(sorted(state_counts.items())),
        "counts_by_schedule_sensitivity": dict(sorted(sensitivity_counts.items())),
        "blocker_counts": dict(sorted(blockers.items())),
        "lane_orders": lane_orders,
        "effects": rows,
        "comparison_contract": {
            "compared_fields": [
                "stable_effect_key",
                "operation_presence",
                "transfer_direction",
                "consumer_role",
                "blocking_relation",
                "resource_lane_order",
                "schedule_sensitivity",
            ],
            "ignored_fields": [
                "absolute_timestamp",
                "observed_duration",
                "scheduler_polling_count",
                "raw_event_count",
                "raw_e2e",
            ],
        },
    }


def compare_predicted_shape(
    model_summary: dict[str, Any],
    oracle: dict[str, Any],
    *,
    sample_limit: int,
    is_self: bool,
    alternate_evidence_ready: bool,
) -> dict[str, Any]:
    """Compare invariant shape directly and gate sensitive cross rows on alternate evidence."""

    decisions = _effect_decisions(model_summary)
    prefetch_readiness_status = _prefetch_readiness_status(model_summary)
    predicted_rows, predicted_lane_orders = _predicted_shape(decisions)
    actual_rows = {
        str(row.get("effect_key")): row
        for row in oracle.get("effects", [])
        if isinstance(row, dict) and row.get("effect_key")
    }
    predicted_by_key = {str(row["effect_key"]): row for row in predicted_rows}
    readiness_cascade_keys = _readiness_cascade_effect_keys(
        predicted_rows,
        actual_rows,
        prefetch_readiness_status,
    )

    blockers: list[str] = []
    if oracle.get("ready") is not True:
        blockers.append("target_shape_oracle_not_ready")
    if not decisions:
        blockers.append("predicted_effect_decisions_missing")
    if blockers:
        return _comparison_result(
            status="not_ready",
            ready=False,
            exact=False,
            blockers=blockers,
            predicted_rows=predicted_rows,
            actual_rows=actual_rows,
            predicted_lane_orders=predicted_lane_orders,
            actual_lane_orders=oracle.get("lane_orders"),
            invariant_mismatches=[],
            schedule_sensitive_mismatches=[],
            readiness_limitation_mismatches=[],
            sample_limit=sample_limit,
            is_self=is_self,
            alternate_evidence_ready=alternate_evidence_ready,
            prefetch_readiness_status=prefetch_readiness_status,
        )

    invariant_mismatches: list[dict[str, Any]] = []
    schedule_sensitive_mismatches: list[dict[str, Any]] = []
    readiness_limitation_mismatches: list[dict[str, Any]] = []
    predicted_keys = set(predicted_by_key)
    actual_keys = set(actual_rows)
    for effect_key in sorted(predicted_keys - actual_keys):
        invariant_mismatches.append(
            {"effect_key": effect_key, "field": "stable_effect_key", "predicted": "present", "actual": "missing"}
        )
    for effect_key in sorted(actual_keys - predicted_keys):
        invariant_mismatches.append(
            {"effect_key": effect_key, "field": "stable_effect_key", "predicted": "missing", "actual": "present"}
        )

    compared_fields = (
        ("target_effect_state", "actual_state", "operation_presence"),
        ("direction", "direction", "transfer_direction"),
        ("consumer_role", "consumer_role", "consumer_role"),
        ("blocking_relation", "blocking_relation", "blocking_relation"),
    )
    for effect_key in sorted(predicted_keys & actual_keys):
        predicted = predicted_by_key[effect_key]
        actual = actual_rows[effect_key]
        predicted_sensitivity = str(predicted.get("schedule_sensitivity") or "")
        actual_sensitivity = str(actual.get("schedule_sensitivity") or "")
        if predicted_sensitivity != actual_sensitivity:
            invariant_mismatches.append(
                {
                    "effect_key": effect_key,
                    "effect_type": predicted.get("effect_type"),
                    "field": "schedule_sensitivity",
                    "predicted": predicted_sensitivity,
                    "actual": actual_sensitivity,
                }
            )
        for predicted_field, actual_field, label in compared_fields:
            if predicted.get(predicted_field) == actual.get(actual_field):
                continue
            mismatch = {
                "effect_key": effect_key,
                "effect_type": predicted.get("effect_type"),
                "field": label,
                "schedule_sensitivity": predicted_sensitivity,
                "predicted": predicted.get(predicted_field),
                "actual": actual.get(actual_field),
            }
            if (
                effect_key in readiness_cascade_keys and label != "transfer_direction"
            ) or _is_prefetch_readiness_limitation(mismatch, prefetch_readiness_status):
                mismatch["limitation"] = _PREFETCH_READINESS_LIMITATION
                readiness_limitation_mismatches.append(mismatch)
            elif label != "transfer_direction" and predicted_sensitivity == "arrival_schedule_sensitive":
                schedule_sensitive_mismatches.append(mismatch)
            else:
                invariant_mismatches.append(mismatch)

    actual_lane_orders = oracle.get("lane_orders") if isinstance(oracle.get("lane_orders"), dict) else {}
    for lane in sorted(set(predicted_lane_orders) | set(actual_lane_orders)):
        predicted_lane = predicted_lane_orders.get(lane, [])
        actual_lane = actual_lane_orders.get(lane, [])
        common_effects = set(predicted_lane) & set(actual_lane)
        predicted_common = [effect_key for effect_key in predicted_lane if effect_key in common_effects]
        actual_common = [effect_key for effect_key in actual_lane if effect_key in common_effects]
        if predicted_common == actual_common:
            continue
        mismatch = {
            "effect_key": "",
            "field": "resource_lane_order",
            "resource_lane": lane,
            "predicted": predicted_common,
            "actual": actual_common,
        }
        common_sensitivities = {
            str(predicted_by_key[effect_key].get("schedule_sensitivity") or "") for effect_key in common_effects
        }
        if common_sensitivities == {"arrival_schedule_sensitive"}:
            schedule_sensitive_mismatches.append(mismatch)
        else:
            invariant_mismatches.append(mismatch)

    prefetch_control_divergence = any(
        effect_key in readiness_cascade_keys
        and str(predicted_by_key.get(effect_key, {}).get("effect_type") or "") == "prefetch_io_operation"
        for effect_key in predicted_keys
    )
    if is_self and prefetch_control_divergence:
        retained_schedule_mismatches: list[dict[str, Any]] = []
        for mismatch in schedule_sensitive_mismatches:
            if str(mismatch.get("effect_type") or "") in {
                "loadback",
                "prefetch_visibility_dependency",
            }:
                mismatch["limitation"] = _PREFETCH_READINESS_LIMITATION
                mismatch["limitation_provenance"] = "prefetch_control_divergence_in_same_self_replay"
                readiness_limitation_mismatches.append(mismatch)
            else:
                retained_schedule_mismatches.append(mismatch)
        schedule_sensitive_mismatches = retained_schedule_mismatches

    schedule_sensitive_count = sum(
        1 for row in predicted_rows if row.get("schedule_sensitivity") == "arrival_schedule_sensitive"
    )
    sensitive_acceptance_ready = is_self or schedule_sensitive_count == 0 or alternate_evidence_ready
    acceptance_mismatches = invariant_mismatches + (schedule_sensitive_mismatches if is_self else [])
    acceptance_ready = not acceptance_mismatches and sensitive_acceptance_ready
    exact = not invariant_mismatches and not schedule_sensitive_mismatches and not readiness_limitation_mismatches
    diagnostic_exact = exact
    if exact:
        status = "ready"
    elif acceptance_ready and readiness_limitation_mismatches:
        status = "ready_with_readiness_limitation"
    elif acceptance_ready:
        status = "ready_with_alternate_evidence"
    else:
        status = "mismatch"
    if not sensitive_acceptance_ready and not acceptance_mismatches:
        status = "alternate_evidence_missing"
    return _comparison_result(
        status=status,
        ready=True,
        exact=exact,
        blockers=[],
        predicted_rows=predicted_rows,
        actual_rows=actual_rows,
        predicted_lane_orders=predicted_lane_orders,
        actual_lane_orders=actual_lane_orders,
        invariant_mismatches=invariant_mismatches,
        schedule_sensitive_mismatches=schedule_sensitive_mismatches,
        readiness_limitation_mismatches=readiness_limitation_mismatches,
        sample_limit=sample_limit,
        is_self=is_self,
        alternate_evidence_ready=alternate_evidence_ready,
        prefetch_readiness_status=prefetch_readiness_status,
    )


def patch_probe_contract_enabled(profile_manifest: dict[str, Any]) -> bool:
    """Return whether profiling explicitly enabled the DAG-patch evidence contract."""

    profiling = profile_manifest.get("profiling") if isinstance(profile_manifest.get("profiling"), dict) else {}
    consumers = profiling.get("python_consumers") if isinstance(profiling.get("python_consumers"), list) else []
    return HICACHE_CONSUMER_DAG_PATCH in consumers


def _load_fact_events(trace_paths: Iterable[Path]) -> tuple[list[dict[str, Any]], list[dict[str, Any]], Counter[str]]:
    events: list[dict[str, Any]] = []
    statuses: list[dict[str, Any]] = []
    errors: Counter[str] = Counter()
    for file_index, path in enumerate(trace_paths):
        rows, status = load_chrome_trace_events(path, auto_repair=True)
        statuses.append(status.to_dict())
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
                    "end": timestamp + duration,
                    "file_index": file_index,
                    "ordinal": ordinal,
                }
            )
    events.sort(key=_event_sort_key)
    return events, statuses, errors


def _build_opportunities(events: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], Counter[str]]:
    opportunities: list[dict[str, Any]] = []
    blockers: Counter[str] = Counter()
    scope_ordinals: dict[str, str] = {}
    role_ordinals: Counter[tuple[str, str]] = Counter()
    for event in events:
        role = str(event["role"])
        descriptors = _EFFECT_DESCRIPTORS.get(role)
        if not descriptors or event["fact_class"] != "workload_identity" or not _completed_opportunity_phase(event):
            continue
        args = event["args"]
        raw_scope = str(args.get("cache_scope") or "")
        span = _object_arg(args.get("full_path_span"))
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
            opportunities.append(
                {
                    "effect_key": f"{effect_type}|{family_key}|{decision_ordinal}",
                    "effect_family_key": family_key,
                    "effect_type": effect_type,
                    "direction": direction,
                    "resource_lane": lane,
                    "request_id": str(args.get("request_id") or ""),
                    "request_identity": request_identity,
                    "source_fact_role": role,
                    "source_fact_ordinal": role_ordinal,
                    "decision_ordinal": decision_ordinal,
                    "cache_scope": scope,
                    "raw_scope": raw_scope,
                    "pid": event["pid"],
                    "tid": event["tid"],
                    "candidate_token_count": end - begin,
                    "anchor": event,
                }
            )
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
        return _actual_row(opportunity, "not_required", evidence=[])
    if len(decisions) != 1:
        return _actual_row(opportunity, "ambiguous", blocker="multiple_loadback_decisions")
    decision = decisions[0]
    effective = _u64(decision["args"].get("effective_token_count"))
    if effective == 0:
        return _actual_row(opportunity, "not_required", evidence=["loadback_decision_observed"])
    node_id = _optional_u64(decision["args"].get("node_id"))
    timings = _timing_calls_for_node(role_events.get("loadback_io_observed", []), node_id, opportunity["pid"])
    if len(timings) != 1:
        state = "ambiguous" if len(timings) > 1 else "unobservable"
        return _actual_row(opportunity, state, blocker="loadback_timing_identity_incomplete")
    state = _transfer_state(effective, _candidate_transfer_token_count(opportunity))
    return _actual_row(
        opportunity,
        state,
        evidence=["loadback_decision_observed", "loadback_io_observed"],
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
        control_events: dict[str, list[dict[str, Any]]] = {}
        evidence: list[str] = []
        for role in ("prefetch_intent_observed", "prefetch_decision_observed", "prefetch_progress_observed"):
            events = _request_events(opportunity, role_events.get(role, []), window_end)
            control_events[role] = events
            if events:
                evidence.append(role)
        progress_results = [
            _optional_bool(event["args"].get("progress_ready"))
            for event in control_events["prefetch_progress_observed"]
        ]
        control_pipeline_limited = bool(
            control_events["prefetch_intent_observed"]
            and control_events["prefetch_decision_observed"]
            and progress_results
            and all(result is True for result in progress_results)
        )
        return _actual_row(
            opportunity,
            "not_required",
            evidence=evidence,
            readiness_limitation=(_PREFETCH_READINESS_LIMITATION if control_pipeline_limited else ""),
        )
    timing = timings[0]
    completed = _u64(timing["args"].get("completed_token_count")) or _u64(timing["args"].get("token_count"))
    state = _transfer_state(completed, _candidate_transfer_token_count(opportunity)) if completed else "required"
    return _actual_row(
        opportunity,
        state,
        evidence=["prefetch_io_observed"],
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
        return _actual_row(opportunity, "not_required", evidence=[])
    if not intents or not progress:
        return _actual_row(opportunity, "unobservable", blocker="prefetch_visibility_progress_missing")
    progress_results = [_optional_bool(event["args"].get("progress_ready")) for event in progress]
    if any(result is None for result in progress_results):
        return _actual_row(opportunity, "unobservable", blocker="prefetch_progress_result_missing")
    if all(progress_results):
        return _actual_row(
            opportunity,
            "not_required",
            evidence=["prefetch_intent_observed", "prefetch_progress_observed", "prefetch_io_observed"],
        )
    consumer = _cache_extend_consumer(opportunity, role_events)
    if consumer is None:
        return _actual_row(opportunity, "unobservable", blocker="prefetch_visibility_consumer_missing")
    return _actual_row(
        opportunity,
        "required",
        evidence=[
            "prefetch_intent_observed",
            "prefetch_progress_observed",
            "prefetch_io_observed",
            "cache_extend_input",
        ],
        actual_consumer_role="cache_extend_input",
    )


def _classify_commit_d2h(opportunity: dict[str, Any], role_events: dict[str, list[dict[str, Any]]]) -> dict[str, Any]:
    enqueues = _nested_events(opportunity["anchor"], role_events.get("commit_device_to_host_enqueue_observed", []))
    enqueues = [event for event in enqueues if str(event["args"].get("phase") or "") == "end"]
    effective = sum(_u64(event["args"].get("effective_token_count")) for event in enqueues)
    if not enqueues or effective == 0:
        return _actual_row(opportunity, "not_required", evidence=[])
    node_ids = {node_id for event in enqueues for node_id in _u64_list(event["args"].get("node_id"))}
    timings = _timing_calls_for_nodes(
        role_events.get("commit_device_to_host_io_observed", []), node_ids, opportunity["pid"]
    )
    if not timings:
        return _actual_row(opportunity, "unobservable", blocker="commit_d2h_timing_identity_incomplete")
    state = _transfer_state(effective, _candidate_transfer_token_count(opportunity))
    return _actual_row(
        opportunity,
        state,
        evidence=["commit_device_to_host_enqueue_observed", "commit_device_to_host_io_observed"],
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
        return _actual_row(
            opportunity, "not_required", evidence=["commit_capacity_release_observed"] if releases else []
        )
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
    completed = sum(
        _u64(event["args"].get("completed_token_count")) or _u64(event["args"].get("token_count")) for event in timings
    )
    state = _transfer_state(completed, _candidate_transfer_token_count(opportunity)) if completed else "required"
    return _actual_row(
        opportunity,
        state,
        evidence=["commit_capacity_release_observed", "writeback_enqueue_observed", "writeback_io_observed"],
        operation_sort_key=min((_event_sort_key(event) for event in timings)),
    )


def _classify_commit_capacity(
    opportunity: dict[str, Any], role_events: dict[str, list[dict[str, Any]]]
) -> dict[str, Any]:
    d2h = _nested_events(opportunity["anchor"], role_events.get("commit_device_to_host_enqueue_observed", []))
    d2h = [event for event in d2h if _u64(event["args"].get("effective_token_count")) > 0]
    if not d2h:
        return _actual_row(opportunity, "not_required", evidence=[])
    releases = _commit_release_calls(opportunity, role_events)
    if not releases:
        return _actual_row(opportunity, "unobservable", blocker="commit_capacity_release_missing")
    consumer = _next_canonical_consumer(opportunity, role_events)
    if consumer is None:
        return _actual_row(
            opportunity,
            "not_required",
            evidence=["commit_capacity_release_observed"],
        )
    return _actual_row(
        opportunity,
        "required",
        evidence=["commit_capacity_release_observed", str(consumer["role"])],
        actual_consumer_role=str(consumer["role"]),
    )


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


def _predicted_shape(decisions: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], dict[str, list[str]]]:
    families: dict[str, dict[str, dict[str, Any]]] = defaultdict(dict)
    for decision in decisions:
        families[str(decision.get("effect_family_key") or "")][str(decision.get("effect_type") or "")] = decision
    rows: list[dict[str, Any]] = []
    lane_rows: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for decision in decisions:
        effect_type = str(decision.get("effect_type") or "")
        state = str(decision.get("target_effect_state") or "unresolved")
        family = families[str(decision.get("effect_family_key") or "")]
        consumer_role, blocking = _predicted_relation(effect_type, state, decision, family)
        row = {
            "effect_key": str(decision.get("effect_key") or ""),
            "effect_family_key": str(decision.get("effect_family_key") or ""),
            "effect_type": effect_type,
            "cache_scope": str(decision.get("cache_scope") or ""),
            "target_effect_state": state,
            "direction": str(decision.get("direction") or ""),
            "consumer_role": consumer_role,
            "blocking_relation": blocking,
            "schedule_sensitivity": str(decision.get("schedule_sensitivity") or "unclassified"),
            "resource_lane": _scoped_resource_lane(
                str(decision.get("cache_scope") or ""),
                str(decision.get("resource_lane") or ""),
            ),
            "eligibility_epoch": _u64(_nested_value(decision, "eligibility_boundary", "epoch")),
            "candidate_pages": [
                str(segment.get("target_page_id") or "")
                for segment in decision.get("candidate_segments", [])
                if isinstance(segment, dict) and segment.get("target_page_id")
            ],
            "effective_pages": [str(page) for page in decision.get("effective_pages", []) if page],
        }
        rows.append(row)
        if state in _ACTIVE_STATES and row["resource_lane"]:
            lane_rows[row["resource_lane"]].append(row)
    lane_orders: dict[str, list[str]] = {}
    for lane, values in sorted(lane_rows.items()):
        values.sort(key=lambda row: (row["eligibility_epoch"], row["effect_key"]))
        lane_orders[lane] = [str(row["effect_key"]) for row in values]
    return rows, lane_orders


def _predicted_relation(
    effect_type: str, state: str, decision: dict[str, Any], family: dict[str, dict[str, Any]]
) -> tuple[str, str]:
    if state not in _ACTIVE_STATES:
        return "none", "none"
    if effect_type == "loadback":
        return "foreground_cache_consumer", "blocking"
    if effect_type == "prefetch_io_operation":
        visibility_state = str(family.get("prefetch_visibility_dependency", {}).get("target_effect_state") or "")
        consumer = "prefetch_visibility_dependency" if visibility_state in _ACTIVE_STATES else "background_completion"
        return consumer, "background"
    if effect_type == "prefetch_visibility_dependency":
        return str(_nested_value(decision, "consumer_boundary", "source_fact_role") or "cache_extend_input"), "blocking"
    if effect_type == "commit_device_to_host":
        if str(family.get("commit_host_to_storage", {}).get("target_effect_state") or "") in _ACTIVE_STATES:
            return "commit_host_to_storage", "family_internal"
        if str(family.get("commit_capacity_gate", {}).get("target_effect_state") or "") in _ACTIVE_STATES:
            return "commit_capacity_gate", "family_internal"
        return "commit_completion", "family_internal"
    if effect_type == "commit_host_to_storage":
        capacity_state = str(family.get("commit_capacity_gate", {}).get("target_effect_state") or "")
        return ("commit_capacity_gate" if capacity_state in _ACTIVE_STATES else "storage_completion"), "family_internal"
    if effect_type == "commit_capacity_gate":
        role = str(_nested_value(decision, "consumer_boundary", "source_fact_role") or "next_capacity_consumer")
        return role, "blocking"
    return "unknown", "unknown"


def _actual_row(
    opportunity: dict[str, Any],
    state: str,
    *,
    evidence: list[str] | None = None,
    blocker: str = "",
    operation_sort_key: tuple[Any, ...] | None = None,
    actual_consumer_role: str = "",
    readiness_limitation: str = "",
) -> dict[str, Any]:
    return {
        "effect_key": opportunity["effect_key"],
        "effect_family_key": opportunity["effect_family_key"],
        "effect_type": opportunity["effect_type"],
        "actual_state": state,
        "direction": opportunity["direction"],
        "resource_lane": _scoped_resource_lane(
            str(opportunity["cache_scope"]),
            str(opportunity["resource_lane"]),
        ),
        "request_identity": opportunity["request_identity"],
        "source_fact_role": opportunity["source_fact_role"],
        "source_fact_ordinal": opportunity["source_fact_ordinal"],
        "decision_ordinal": opportunity["decision_ordinal"],
        "schedule_sensitivity": _schedule_sensitivity(str(opportunity["effect_type"])),
        "evidence": evidence or [],
        "blocker": blocker,
        "readiness_limitation": readiness_limitation,
        "operation_sort_key": operation_sort_key,
        "actual_consumer_role": actual_consumer_role,
    }


def _comparison_result(
    *,
    status: str,
    ready: bool,
    exact: bool,
    blockers: list[str],
    predicted_rows: list[dict[str, Any]],
    actual_rows: dict[str, dict[str, Any]],
    predicted_lane_orders: dict[str, list[str]],
    actual_lane_orders: Any,
    invariant_mismatches: list[dict[str, Any]],
    schedule_sensitive_mismatches: list[dict[str, Any]],
    readiness_limitation_mismatches: list[dict[str, Any]],
    sample_limit: int,
    is_self: bool,
    alternate_evidence_ready: bool,
    prefetch_readiness_status: str,
) -> dict[str, Any]:
    mismatches = invariant_mismatches + schedule_sensitive_mismatches + readiness_limitation_mismatches
    schedule_sensitive_count = sum(
        1 for row in predicted_rows if row.get("schedule_sensitivity") == "arrival_schedule_sensitive"
    )
    return {
        "schema": "trace_sim.hicache.effect_shape_comparison.v2",
        "status": status,
        "ready": ready,
        "exact": exact,
        "acceptance_ready": ready
        and not invariant_mismatches
        and (not schedule_sensitive_mismatches or not is_self)
        and (is_self or schedule_sensitive_count == 0 or alternate_evidence_ready),
        "predicted_effect_count": len(predicted_rows),
        "actual_effect_count": len(actual_rows),
        "mismatch_count": len(mismatches),
        "acceptance_mismatch_count": len(invariant_mismatches) + (len(schedule_sensitive_mismatches) if is_self else 0),
        "invariant_mismatch_count": len(invariant_mismatches),
        "schedule_sensitive_mismatch_count": len(schedule_sensitive_mismatches),
        "readiness_limitation_mismatch_count": len(readiness_limitation_mismatches),
        "prefetch_readiness_status": prefetch_readiness_status,
        "schedule_sensitive_count": schedule_sensitive_count,
        "schedule_invariant_count": len(predicted_rows) - schedule_sensitive_count,
        "alternate_evidence_required": not is_self and schedule_sensitive_count > 0,
        "alternate_evidence_ready": is_self or schedule_sensitive_count == 0 or alternate_evidence_ready,
        "diagnostic_exact": not mismatches,
        "blockers": blockers,
        "mismatch_samples": mismatches[:sample_limit],
        "predicted_lane_orders": predicted_lane_orders,
        "actual_lane_orders": actual_lane_orders if isinstance(actual_lane_orders, dict) else {},
        "raw_e2e_used": False,
        "absolute_timing_used": False,
        "polling_count_used": False,
    }


def _schedule_sensitivity(effect_type: str) -> str:
    """Classify whether closed-loop request spacing can change direct effect shape."""

    return "arrival_schedule_sensitive" if effect_type in _ARRIVAL_SCHEDULE_SENSITIVE_EFFECTS else "schedule_invariant"


def _is_prefetch_readiness_limitation(mismatch: dict[str, Any], readiness_status: str) -> bool:
    """Recognize only shape fields directly controlled by the unmodeled prefetch pipeline."""

    if readiness_status != _PREFETCH_READINESS_LIMITATION:
        return False
    effect_type = str(mismatch.get("effect_type") or "")
    field = str(mismatch.get("field") or "")
    if effect_type == "prefetch_visibility_dependency":
        return field in {"operation_presence", "consumer_role", "blocking_relation"}
    if effect_type == "prefetch_io_operation" and field == "consumer_role":
        relation_values = {str(mismatch.get("predicted") or ""), str(mismatch.get("actual") or "")}
        return bool(relation_values & {"background_completion", "prefetch_visibility_dependency"})
    return False


def _readiness_cascade_effect_keys(
    predicted_rows: list[dict[str, Any]],
    actual_rows: dict[str, dict[str, Any]],
    readiness_status: str,
) -> set[str]:
    """Find effects whose shape differs only through modeled prefetch visibility."""

    if readiness_status != _PREFETCH_READINESS_LIMITATION:
        return set()
    limited: set[str] = set()
    predicted_by_family: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in predicted_rows:
        predicted_by_family[str(row.get("effect_family_key") or "")].append(row)
        actual = actual_rows.get(str(row.get("effect_key") or ""))
        if actual is None or not _shape_fields_differ(row, actual):
            continue
        effect_type = str(row.get("effect_type") or "")
        if effect_type == "prefetch_io_operation" and _prefetch_uses_unmodeled_control_pipeline(row, actual):
            limited.add(str(row["effect_key"]))
        elif effect_type == "loadback" and _loadback_uses_prefetch_visibility(row, predicted_rows):
            limited.add(str(row["effect_key"]))
        elif effect_type in {
            "commit_device_to_host",
            "commit_host_to_storage",
        } and _commit_omits_prefetch_visible_pages(
            row,
            actual,
            predicted_rows,
        ):
            limited.add(str(row["effect_key"]))

    limited_families = {
        str(row.get("effect_family_key") or "")
        for rows in predicted_by_family.values()
        for row in rows
        if str(row.get("effect_key") or "") in limited and str(row.get("effect_type") or "").startswith("commit_")
    }
    for family in limited_families:
        for row in predicted_by_family.get(family, []):
            actual = actual_rows.get(str(row.get("effect_key") or ""))
            if actual is not None and _shape_fields_differ(row, actual):
                limited.add(str(row["effect_key"]))
    return limited


def _prefetch_uses_unmodeled_control_pipeline(
    predicted: dict[str, Any],
    actual: dict[str, Any],
) -> bool:
    """Recognize payload submission predicted across an unmodeled query/revoke boundary."""

    return bool(
        str(predicted.get("target_effect_state") or "") in _ACTIVE_STATES
        and _page_set(predicted, "effective_pages")
        and str(actual.get("actual_state") or "") == "not_required"
        and str(actual.get("readiness_limitation") or "") == _PREFETCH_READINESS_LIMITATION
    )


def _shape_fields_differ(predicted: dict[str, Any], actual: dict[str, Any]) -> bool:
    return any(
        predicted.get(predicted_field) != actual.get(actual_field)
        for predicted_field, actual_field in (
            ("target_effect_state", "actual_state"),
            ("direction", "direction"),
            ("consumer_role", "consumer_role"),
            ("blocking_relation", "blocking_relation"),
        )
    )


def _loadback_uses_prefetch_visibility(
    loadback: dict[str, Any],
    predicted_rows: list[dict[str, Any]],
) -> bool:
    effective_pages = _page_set(loadback, "effective_pages")
    if not effective_pages:
        return False
    prior_visible = _prior_prefetch_pages(loadback, predicted_rows, "effective_pages")
    return bool(prior_visible) and effective_pages <= prior_visible


def _commit_omits_prefetch_visible_pages(
    commit: dict[str, Any],
    actual: dict[str, Any],
    predicted_rows: list[dict[str, Any]],
) -> bool:
    if str(actual.get("actual_state") or "") not in _ACTIVE_STATES:
        return False
    candidate_pages = _page_set(commit, "candidate_pages")
    effective_pages = _page_set(commit, "effective_pages")
    omitted_pages = candidate_pages - effective_pages
    if not omitted_pages:
        return False
    prior_visible = _prior_prefetch_pages(commit, predicted_rows, "effective_pages")
    return bool(prior_visible) and omitted_pages <= prior_visible


def _prior_prefetch_pages(
    consumer: dict[str, Any],
    predicted_rows: list[dict[str, Any]],
    page_field: str,
) -> set[str]:
    scope = str(consumer.get("cache_scope") or "")
    epoch = int(consumer.get("eligibility_epoch") or 0)
    pages: set[str] = set()
    for row in predicted_rows:
        if str(row.get("effect_type") or "") != "prefetch_visibility_dependency":
            continue
        if str(row.get("cache_scope") or "") != scope:
            continue
        if int(row.get("eligibility_epoch") or 0) >= epoch:
            continue
        pages.update(_page_set(row, page_field))
    return pages


def _page_set(row: dict[str, Any], field: str) -> set[str]:
    values = row.get(field)
    if not isinstance(values, list):
        return set()
    return {str(value) for value in values if value}


def _effect_decisions(model_summary: dict[str, Any]) -> list[dict[str, Any]]:
    modules = model_summary.get("modules") if isinstance(model_summary.get("modules"), list) else []
    for module in modules:
        if not isinstance(module, dict) or module.get("name") != "HiCacheModule":
            continue
        hicache = module.get("hicache") if isinstance(module.get("hicache"), dict) else {}
        ledger = hicache.get("effect_decisions") if isinstance(hicache.get("effect_decisions"), dict) else {}
        decisions = ledger.get("decisions") if isinstance(ledger.get("decisions"), list) else []
        return [decision for decision in decisions if isinstance(decision, dict)]
    return []


def _prefetch_readiness_status(model_summary: dict[str, Any]) -> str:
    """Read the compact model capability status without consulting target actual shape."""

    modules = model_summary.get("modules") if isinstance(model_summary.get("modules"), list) else []
    for module in modules:
        if not isinstance(module, dict) or module.get("name") != "HiCacheModule":
            continue
        hicache = module.get("hicache") if isinstance(module.get("hicache"), dict) else {}
        ledger = hicache.get("effect_decisions") if isinstance(hicache.get("effect_decisions"), dict) else {}
        return str(ledger.get("prefetch_readiness_status") or "")
    return ""


def _index_roles(events: list[dict[str, Any]]) -> dict[str, list[dict[str, Any]]]:
    result: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for event in events:
        result[str(event["role"])].append(event)
    return result


def _request_events(opportunity: dict[str, Any], events: list[dict[str, Any]], window_end: int) -> list[dict[str, Any]]:
    request_id = str(opportunity["request_id"])
    start = int(opportunity["anchor"]["ts"])
    return [
        event
        for event in events
        if event["pid"] == opportunity["pid"]
        and str(event["args"].get("request_id") or "") == request_id
        and start <= event["ts"] <= window_end
    ]


def _request_window_end(
    opportunity: dict[str, Any], opportunities: list[dict[str, Any]], role_events: dict[str, list[dict[str, Any]]]
) -> int:
    consumer = _cache_extend_consumer(opportunity, role_events)
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


def _prefetch_operation_window_end(
    opportunity: dict[str, Any],
    opportunities: list[dict[str, Any]],
) -> int:
    """Keep payload observation open after cache extend until a repeated candidate."""

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


def _cache_extend_consumer(
    opportunity: dict[str, Any], role_events: dict[str, list[dict[str, Any]]]
) -> dict[str, Any] | None:
    request_id = str(opportunity["request_id"])
    start = int(opportunity["anchor"]["ts"])
    candidates = [
        event
        for event in role_events.get("cache_extend_input", [])
        if event["pid"] == opportunity["pid"] and event["ts"] >= start and request_id in _request_ids(event["args"])
    ]
    return min(candidates, key=_event_sort_key) if candidates else None


def _next_canonical_consumer(
    opportunity: dict[str, Any], role_events: dict[str, list[dict[str, Any]]]
) -> dict[str, Any] | None:
    anchor_key = _canonical_boundary_sort_key(opportunity["anchor"])
    candidates = [
        event
        for role in (*_EFFECT_DESCRIPTORS, "cache_extend_input")
        for event in role_events.get(role, [])
        if event["fact_class"] == "workload_identity"
        and _completed_state_fact_phase(event)
        and event["pid"] == opportunity["pid"]
        and str(event["args"].get("cache_scope") or "") == opportunity["raw_scope"]
        and _canonical_boundary_sort_key(event) > anchor_key
    ]
    return min(candidates, key=_canonical_boundary_sort_key) if candidates else None


def _nested_events(parent: dict[str, Any], events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [event for event in events if _nested(event, parent)]


def _nested(child: dict[str, Any], parent: dict[str, Any]) -> bool:
    return (
        child["pid"] == parent["pid"]
        and child["tid"] == parent["tid"]
        and child["ts"] >= parent["ts"]
        and child["end"] <= parent["end"]
    )


def _commit_release_calls(
    opportunity: dict[str, Any], role_events: dict[str, list[dict[str, Any]]]
) -> list[dict[str, Any]]:
    enqueues = _nested_events(opportunity["anchor"], role_events.get("commit_device_to_host_enqueue_observed", []))
    node_ids = {
        node_id
        for event in enqueues
        if _u64(event["args"].get("effective_token_count")) > 0
        for node_id in _u64_list(event["args"].get("node_id"))
    }
    starts = [
        event
        for event in role_events.get("commit_capacity_release_observed", [])
        if event["pid"] == opportunity["pid"]
        and str(event["args"].get("phase") or "") == "start"
        and node_ids.intersection(_u64_list(event["args"].get("operation_node_ids")))
    ]
    return [
        end
        for start in starts
        if (end := _paired_end(start, role_events.get("commit_capacity_release_observed", []))) is not None
    ]


def _timing_calls_for_node(events: list[dict[str, Any]], node_id: int | None, pid: str) -> list[dict[str, Any]]:
    return _timing_calls_for_nodes(events, set() if node_id is None else {node_id}, pid)


def _timing_calls_for_nodes(events: list[dict[str, Any]], node_ids: set[int], pid: str) -> list[dict[str, Any]]:
    if not node_ids:
        return []
    starts = [
        event
        for event in events
        if event["pid"] == pid
        and str(event["args"].get("phase") or "") == "start"
        and node_ids.intersection(_u64_list(event["args"].get("operation_node_ids")))
    ]
    return [end for start in starts if (end := _paired_end(start, events)) is not None and end["dur"] > 0]


def _paired_end(start: dict[str, Any], events: list[dict[str, Any]]) -> dict[str, Any] | None:
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


def _transfer_state(effective: int, candidate: int) -> str:
    if effective <= 0:
        return "not_required"
    if candidate > 0 and effective < candidate:
        return "partial"
    return "required"


def _candidate_transfer_token_count(opportunity: dict[str, Any]) -> int:
    """Return the full-page candidate span used by the target pager."""

    candidate = int(opportunity["candidate_token_count"])
    page_size = _u64(opportunity["anchor"]["args"].get("source_page_size"))
    if page_size == 0:
        return candidate
    return candidate - candidate % page_size


def _scoped_resource_lane(scope: str, lane: str) -> str:
    """Return the resource identity used for deterministic per-rank ordering."""

    if not scope or not lane:
        return ""
    return f"{scope}/{lane}"


def _completed_opportunity_phase(event: dict[str, Any]) -> bool:
    phase = str(event["args"].get("phase") or "").lower()
    return phase == "end"


def _completed_state_fact_phase(event: dict[str, Any]) -> bool:
    phase = str(event["args"].get("phase") or "").lower()
    return phase == ("start" if event["role"] == "cache_extend_input" else "end")


def _canonical_boundary_sort_key(event: dict[str, Any]) -> tuple[Any, ...]:
    """Order state facts by the boundary timestamp consumed by the C++ model."""

    phase = str(event["args"].get("phase") or "").lower()
    boundary_timestamp = int(event["end"]) if phase == "end" else int(event["ts"])
    return (boundary_timestamp, *_event_sort_key(event)[1:])


def _event_sort_key(event: dict[str, Any]) -> tuple[Any, ...]:
    return (
        int(event["ts"]),
        str(event["pid"]),
        str(event["tid"]),
        str(event["name"]),
        int(event["file_index"]),
        int(event["ordinal"]),
    )


def _request_ids(args: dict[str, Any]) -> set[str]:
    value = args.get("request_ids", args.get("batch_request_ids", []))
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except json.JSONDecodeError:
            return set()
    return {str(item) for item in value} if isinstance(value, list) else set()


def _object_arg(value: Any) -> dict[str, Any]:
    if isinstance(value, dict):
        return value
    if isinstance(value, str):
        try:
            parsed = json.loads(value)
        except json.JSONDecodeError:
            return {}
        return parsed if isinstance(parsed, dict) else {}
    return {}


def _u64_list(value: Any) -> list[int]:
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except json.JSONDecodeError:
            value = [value]
    if not isinstance(value, list):
        value = [value]
    result: list[int] = []
    for item in value:
        number = _optional_u64(item)
        if number is not None:
            result.append(number)
    return result


def _u64(value: Any) -> int:
    number = _optional_u64(value)
    return number if number is not None else 0


def _optional_u64(value: Any) -> int | None:
    try:
        number = int(value)
    except (TypeError, ValueError):
        return None
    return number if number >= 0 else None


def _optional_bool(value: Any) -> bool | None:
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


def _nested_value(payload: dict[str, Any], object_key: str, value_key: str) -> Any:
    value = payload.get(object_key)
    return value.get(value_key) if isinstance(value, dict) else None
