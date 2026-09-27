"""Bind forward scopes and compare same-base CPU service outside synchronization."""

from collections import defaultdict
import math


def bind_forward_steps(report: dict, events, expected_ranks) -> list[dict]:
    """Keep measured scopes intact; never infer request identity from timestamps."""
    requests = [row for row in report["requests"] if row.get("measure") and row.get("kind") == "request"]
    by_id = {row["logical_request_id"]: row for row in requests}
    ranks = set(expected_ranks)
    if not requests or len(by_id) != len(requests) or not ranks:
        raise ValueError("forward binding requires unique formal requests and declared ranks")
    for left, right in zip(requests, requests[1:]):
        if left["end_time_ms"] > right["start_time_ms"]:
            raise ValueError("forward CPU calibration currently requires serial requests")
    begin = round(report["formal_window"]["formal_begin_ms"] * 1_000_000)
    end = round(report["formal_window"]["formal_end_ms"] * 1_000_000)
    rows = []
    covered = {request: set() for request in by_id}
    for event in events:
        if not event["name"].startswith("step["):
            continue
        if event["end_ns"] <= begin or event["start_ns"] >= end:
            continue
        identity = event.get("identity") or {}
        ids = identity.get("request_ids", [])
        rank = identity.get("tp_rank")
        if len(ids) != 1 or ids[0] not in by_id or rank not in ranks:
            raise ValueError("forward step lacks a declared request/rank identity")
        if not event["returned"] or event["end_ns"] <= event["start_ns"]:
            raise ValueError("forward step is incomplete or has an invalid interval")
        cpu = event.get("thread_cpu_ns")
        if type(cpu) is not int or cpu < 0:
            raise ValueError("forward step lacks measured thread CPU time")
        rows.append(dict(event, request_id=ids[0]))
        covered[ids[0]].add(rank)
    if any(actual != ranks for actual in covered.values()):
        raise ValueError("forward timing does not cover every formal request and declared rank")
    counts = defaultdict(int)
    rows.sort(key=lambda row: row["start_ns"])
    for row in rows:
        key = (row["request_id"], row["identity"]["tp_rank"], row["name"].split(" ", 1)[0])
        row["forward_ordinal"] = counts[key]
        counts[key] += 1
    return rows


def observe_forward_cpu(document: dict, syncs: list[dict]) -> dict:
    """Separate timed same-thread synchronization from explicit forward steps.

    Missing synchronization observations are not interpreted as zero CPU work.
    The caller supplies admitted steps and synchronization_scopes in nanoseconds.
    """
    by_thread = defaultdict(list)
    for event in syncs:
        by_thread[(event["pid"], event["tid"])].append(event)
    rows = []
    for step in document["rows"]:
        lo, hi = step["start_ns"], step["end_ns"]
        thread = (step["pid"], step["tid"])
        selected = sorted(
            (event for event in by_thread[thread] if event["start_ns"] < hi and event["end_ns"] > lo),
            key=lambda event: event["start_ns"],
        )
        if not selected:
            raise ValueError(f"missing timed synchronization in forward step: {step['request_id']}")
        end = lo
        for event in selected:
            if event["start_ns"] < end or event["end_ns"] > hi or event["end_ns"] < event["start_ns"]:
                raise ValueError("synchronization intervals overlap or cross the forward boundary")
            cpu = event["thread_cpu_ns"]
            if type(cpu) is not int or not 0 <= cpu <= event["end_ns"] - event["start_ns"] + 1000:
                raise ValueError("synchronization lacks valid thread CPU timing")
            end = event["end_ns"]
        sync_cpu = sum(event["thread_cpu_ns"] for event in selected) / 1000
        outside_cpu = step["thread_cpu_ns"] / 1000 - sync_cpu
        if not math.isfinite(outside_cpu) or outside_cpu < 0:
            raise ValueError("synchronization CPU exceeds measured forward CPU")
        row = dict(
            request_id=step["request_id"],
            rank=step["identity"]["tp_rank"],
            forward_ordinal=step.get("forward_ordinal", 0),
            phase=step["name"].split(" ", 1)[0],
            outside_sync_cpu_us=outside_cpu,
            sync_count=len(selected),
        )
        rows.append(row)
    return {"rows": rows}


def compare_forward_cpu(light: dict, profiled: dict) -> dict:
    """Match explicit identities, retaining signed differences and input order.

    Callers must establish that both captures use the same base configuration
    and token plan. Matching rows alone cannot establish measurement provenance.
    Inputs come from bind_forward_steps followed by observe_forward_cpu:
    identities are unique and service values are finite and nonnegative.
    This stage checks differences between the two captures, not their admission again.
    """

    def index(document: dict) -> dict[tuple, dict]:
        return {
            (row["request_id"], row["rank"], row["phase"], row.get("forward_ordinal", 0)): row
            for row in document["rows"]
        }

    light_rows, profiled_rows = index(light), index(profiled)
    if light_rows.keys() != profiled_rows.keys():
        raise ValueError("paired forward CPU measurements have different identities")
    rows = []
    for key, normal in light_rows.items():
        full = profiled_rows[key]
        if normal["sync_count"] != full["sync_count"]:
            raise ValueError(f"paired synchronization counts differ: {key}")
        values = normal["outside_sync_cpu_us"], full["outside_sync_cpu_us"]
        rows.append(
            dict(
                request_id=key[0],
                rank=key[1],
                phase=key[2],
                forward_ordinal=key[3],
                delta={"outside_sync_cpu_us": values[1] - values[0]},
            )
        )
    return {"rows": rows}
