"""Remove measured recorder work without subtracting overlapping CPU budgets.

Only measured source intervals are corrected; unmeasured overhead is retained.
Target timings never enter these corrections.
"""

from bisect import bisect_left, bisect_right
from collections import defaultdict
from copy import deepcopy
from typing import Any, Iterable


def hook_writes(events: Iterable[dict[str, Any]], begin_ns: int, end_ns: int) -> list[dict[str, Any]]:
    """Read completed same-thread logger measurements from an opted-in capture.

    Coordinates include lock waiting, but only measured thread CPU is removable.
    The final unreported write and wrapper work outside LogEvent stay uncorrected.
    Missing measurements in the selected window are an input error, not zero cost.
    """
    previous = {}
    rows = []
    for event in events:
        if event.get("ph") != "X":
            continue

        lane = event["pid"], event["tid"]
        writer = previous.get(lane)
        previous[lane] = event
        emission = event.get("hook_recorder_previous")
        if emission is None:
            continue

        start, stop, cpu = emission["start_ns"], emission["end_ns"], emission["thread_cpu_ns"]
        if not start < stop or not 0 <= cpu <= stop - start:
            raise ValueError("Invalid hook recorder CPU measurement")
        # A nested hook can be written before its enclosing API finishes. The
        # carrier's end, not its start, bounds the preceding completed write.
        if (
            writer is None
            or (writer["ts"] + writer["dur"]) * 1000 > start
            or stop > (event["ts"] + event["dur"] + 1) * 1000
        ):
            raise ValueError("Hook recorder measurement is not between same-thread writes")
        if begin_ns <= start and stop <= end_ns:
            rows.append(dict(pid=lane[0], tid=lane[1], begin_ns=start, end_ns=stop, thread_cpu_ns=cpu))

    if not rows:
        raise ValueError("Hook emission timing was declared but no complete measurements cover the formal window")
    return rows


def clean_reference(records):
    emissions = defaultdict(dict)
    for row in records:
        emission = row.get("previous_emission")
        if emission is None:
            continue
        lane = emissions[row["pid"], row["tid"]]
        key = emission["start_ns"], emission["end_ns"]
        if key in lane and lane[key] != emission:
            raise ValueError("Inconsistent duplicate recorder observation")
        lane[key] = emission
    if not emissions:
        raise ValueError("Reference has no measured recorder observations")
    indexed = {}
    for lane, unique in emissions.items():
        ordered = sorted(unique.values(), key=lambda r: r["start_ns"])
        prefix, end = [0], -1
        for row in ordered:
            if row["start_ns"] < end or row["end_ns"] <= row["start_ns"]:
                raise ValueError("Recorder intervals overlap or are invalid")
            cpu = row["thread_cpu_ns"]
            if not 0 <= cpu <= row["end_ns"] - row["start_ns"]:
                raise ValueError("Invalid recorder CPU measurement")
            prefix.append(prefix[-1] + cpu)
            end = row["end_ns"]
        indexed[lane] = ([r["start_ns"] for r in ordered], [r["end_ns"] for r in ordered], prefix)

    def clean(row, lane):
        starts, ends, prefix = indexed.get(lane, ([], [], [0]))
        a, b = row["start_ns"], row["end_ns"]
        left, right = bisect_right(ends, a), bisect_left(starts, b)
        if left < right and (starts[left] < a or ends[right - 1] > b):
            raise ValueError("Recorder write crosses a CPU envelope")
        removed = prefix[right] - prefix[left]
        if removed > row["thread_cpu_ns"]:
            raise ValueError("Recorder CPU exceeds measured scope")
        row["measured_recorder_cpu_ns"] = removed
        row["thread_cpu_ns"] -= removed

    result = deepcopy(records)
    for row in result:
        lane = row["pid"], row["tid"]
        clean(row, lane)
        if row["name"] == "hicache.layer_wait_batch":
            for call in row["identity"]["calls"]:
                clean(call, lane)
    return result


def source_writes(records, begin_ns, end_ns):
    previous, seen, rows = {}, set(), []
    for timer in records:
        lane = timer["pid"], timer["tid"]
        writer = previous.get(lane)
        previous[lane] = timer
        emission = timer.get("previous_emission")
        if emission is None or not (begin_ns <= emission["start_ns"] and emission["end_ns"] <= end_ns):
            continue
        if writer is None or not writer["end_ns"] <= emission["start_ns"] < emission["end_ns"] <= timer["end_ns"]:
            raise ValueError("Write cannot be bound to preceding same-thread record")
        key = (*lane, emission["start_ns"], emission["end_ns"])
        if key in seen:
            raise ValueError("Duplicate source write")
        seen.add(key)
        rows.append(
            dict(
                pid=lane[0],
                tid=lane[1],
                method=writer["name"],
                method_begin_ns=emission["start_ns"],
                method_end_ns=emission["end_ns"],
                thread_cpu_ns=emission["thread_cpu_ns"],
            )
        )
    return rows
