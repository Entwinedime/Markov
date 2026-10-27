"""Disjoint measured child scopes and synchronization owned by a CPU envelope."""

from bisect import bisect_left, bisect_right
from collections.abc import Iterable
from collections import defaultdict
from dataclasses import dataclass


@dataclass
class _ThreadScopes:
    events: list[dict]
    starts_ns: list[int | float]
    prefix_ends_ns: list[int | float]


class ThreadIntervalIndex:
    """Index measured CPU intervals by original PID/TID and nanoseconds.

    Prefix maxima retain enclosing or overlapping observations so callers still
    reject invalid boundaries; an index query does not validate or clip scopes.
    """

    def __init__(self, events: Iterable[dict]) -> None:
        by_thread: dict[tuple[int, int], list[dict]] = defaultdict(list)
        for event in events:
            by_thread[event["pid"], event["tid"]].append(event)

        self._threads: dict[tuple[int, int], _ThreadScopes] = {}
        for thread, rows in by_thread.items():
            rows.sort(key=lambda row: row["start_ns"])
            ends: list[int | float] = []
            frontier: int | float = 0
            for row in rows:
                frontier = max(frontier, row["end_ns"])
                ends.append(frontier)

            self._threads[thread] = _ThreadScopes(rows, [row["start_ns"] for row in rows], ends)

    def overlapping(self, parent: dict) -> list[dict]:
        thread = self._threads.get((parent["pid"], parent["tid"]))
        if thread is None:
            return []

        lo, hi = parent["start_ns"], parent["end_ns"]
        first = bisect_right(thread.prefix_ends_ns, lo)
        last = bisect_left(thread.starts_ns, hi)
        return [row for row in thread.events[first:last] if row["end_ns"] > lo]

    def starting_in(self, parent: dict) -> list[dict]:
        """Select starts in the half-open parent range, retaining crossing ends."""

        thread = self._threads.get((parent["pid"], parent["tid"]))
        if thread is None:
            return []

        first = bisect_left(thread.starts_ns, parent["start_ns"])
        last = bisect_left(thread.starts_ns, parent["end_ns"])
        return thread.events[first:last]


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


def synchronization_scopes(events: Iterable[dict]) -> ThreadIntervalIndex:
    """Normalize LD synchronization once per capture for every CPU observer.

    PID/TID are integers; interval and thread CPU values are nanoseconds.
    A missing CPU measurement remains None, never an invented zero.
    """
    return ThreadIntervalIndex(
        [
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
    )


def exclusive_cpu_parts(parent: dict, candidates: list[dict], syncs: ThreadIntervalIndex) -> dict:
    children = [row for row in candidates if overlaps(row, parent)]
    if any(not contains(parent, row) or not row["returned"] for row in children):
        raise ValueError("Measured child is incomplete or crosses its parent CPU scope")
    outer = [row for row in children if not any(other is not row and contains(other, row) for other in children)]
    outer.sort(key=lambda row: row["start_ns"])
    if any(a["end_ns"] > b["start_ns"] for a, b in zip(outer, outer[1:])):
        raise ValueError("Outermost measured children overlap")
    external = []
    for sync in syncs.overlapping(parent):
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
