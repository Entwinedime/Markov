"""Disjoint measured child scopes and synchronization owned by a CPU envelope."""

from collections.abc import Iterable
from collections import defaultdict


def paired_service(reference: dict, profiled: dict) -> dict:
    """Describe one identity-matched CPU budget in the profiled source's coordinates.

    Callers establish matching request work and child scopes. Service excludes
    child/synchronization CPU; signed deltas preserve a slower reference.
    """

    normal = reference["exclusive_service_cpu_us"]
    measured = profiled["exclusive_service_cpu_us"]
    identity = profiled["identity"]
    return dict(
        request_id=identity["request_ids"][0],
        rank=identity["tp_rank"],
        pid=profiled["pid"],
        tid=profiled["tid"],
        method=profiled["name"],
        operation_id=None,
        method_begin_ns=profiled["start_ns"],
        method_end_ns=profiled["end_ns"],
        exclusive_ranges_ns=profiled["exclusive_ranges_ns"],
        normal_service_cpu_us=normal,
        profiled_service_cpu_us=measured,
        measured_service_delta_us=measured - normal,
    )


def validate_host_ranges(rows: Iterable[dict]) -> None:
    """Reject duplicate CPU ownership on a source thread; touching ranges are allowed."""

    lanes = defaultdict(list)
    for row in rows:
        lanes[str(row["pid"]), str(row["tid"])].extend(row["exclusive_ranges_ns"])

    for spans in lanes.values():
        spans.sort()
        if any(left[1] > right[0] for left, right in zip(spans, spans[1:])):
            raise ValueError("Measured host CPU scopes overlap")


def contains(a: dict, b: dict) -> bool:
    return (
        (a["pid"], a["tid"]) == (b["pid"], b["tid"]) and a["start_ns"] <= b["start_ns"] and b["end_ns"] <= a["end_ns"]
    )


def overlaps(a: dict, b: dict) -> bool:
    return (a["pid"], a["tid"]) == (b["pid"], b["tid"]) and a["start_ns"] < b["end_ns"] and b["start_ns"] < a["end_ns"]


def synchronization_scopes(events: Iterable[dict]) -> list[dict]:
    """Normalize LD synchronization once per capture for every CPU observer.

    PID/TID are integers; interval and thread CPU values are nanoseconds.
    A missing CPU measurement remains None, never an invented zero.
    """
    return [
        dict(
            pid=int(e["pid"]),
            tid=int(e["tid"]),
            start_ns=e["ts"] * 1000,
            end_ns=(e["ts"] + e["dur"]) * 1000,
            thread_cpu_ns=e.get("args", {}).get("thread_cpu_ns"),
        )
        for e in events
        if e.get("ph") == "X" and e.get("name", "").startswith("AscendCL@aclrtSynchronize")
    ]


def exclusive_cpu_parts(parent: dict, candidates: list[dict], syncs: list[dict]) -> dict:
    children = [row for row in candidates if overlaps(row, parent)]
    if any(not contains(parent, row) or not row["returned"] for row in children):
        raise ValueError("Measured child is incomplete or crosses its parent CPU scope")
    outer = [row for row in children if not any(other is not row and contains(other, row) for other in children)]
    outer.sort(key=lambda row: row["start_ns"])
    if any(a["end_ns"] > b["start_ns"] for a, b in zip(outer, outer[1:])):
        raise ValueError("Outermost measured children overlap")
    external = []
    for sync in syncs:
        if not overlaps(parent, sync):
            continue
        if not contains(parent, sync):
            raise ValueError("Synchronization crosses parent CPU scope")
        if any(contains(child, sync) for child in outer):
            continue
        if any(overlaps(child, sync) for child in outer):
            raise ValueError("Synchronization crosses measured child scope")
        external.append(sync)
    external.sort(key=lambda row: row["start_ns"])
    if any(a["end_ns"] > b["start_ns"] for a, b in zip(external, external[1:])):
        raise ValueError("Synchronization observations overlap")

    ranges, cursor = [], parent["start_ns"]
    for child in outer:
        if cursor < child["start_ns"]:
            ranges.append([cursor, child["start_ns"]])
        cursor = child["end_ns"]
    if cursor < parent["end_ns"]:
        ranges.append([cursor, parent["end_ns"]])
    return dict(
        child_calls=len(outer),
        child_cpu_ns=sum(row["thread_cpu_ns"] for row in outer),
        sync_calls=len(external),
        sync_cpu_ns=sum(row["thread_cpu_ns"] for row in external),
        exclusive_ranges_ns=ranges,
    )
