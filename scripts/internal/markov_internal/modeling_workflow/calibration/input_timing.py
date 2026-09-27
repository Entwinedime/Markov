"""Measured input-processing CPU, paired by request/rank rather than wall time."""

from collections import Counter
from decimal import Decimal

from .cpu_scopes import exclusive_cpu_parts, paired_service


def bind_input_calls(report: dict, timers: list[dict], expected_ranks) -> list[dict]:
    requests = [row for row in report["requests"] if row.get("measure") and row.get("kind") == "request"]
    identities = {row["logical_request_id"] for row in requests}
    ranks = set(expected_ranks)
    if not requests or len(identities) != len(requests) or not ranks:
        raise ValueError("Input timing requires unique formal requests and declared ranks")
    lo, hi = [Decimal(str(report["formal_window"][key])) * 1000000 for key in ("formal_begin_ms", "formal_end_ms")]
    rows = []
    for row in timers:
        if row["name"] != "scheduler.process_input_requests":
            continue
        ids = row["identity"]["request_ids"]
        if not identities.intersection(ids):
            continue
        if (
            len(ids) != 1
            or ids[0] not in identities
            or not row["returned"]
            or not lo <= row["start_ns"] <= row["end_ns"] <= hi
        ):
            raise ValueError("Input timing needs a complete single-request call inside the formal window")
        rows.append(row)
    coverage = Counter((row["identity"]["request_ids"][0], row["identity"]["tp_rank"]) for row in rows)
    if coverage != Counter({(request, rank): 1 for request in identities for rank in ranks}):
        raise ValueError("Input timing does not uniquely cover every formal request/rank")
    return rows


def observe_input_cpu(rows: list[dict], timers: list[dict], syncs: list[dict]) -> list[dict]:
    measured = []
    for parent in rows:
        parts = exclusive_cpu_parts(parent, [row for row in timers if row["name"] != parent["name"]], syncs)
        own_cpu = parent["thread_cpu_ns"] - parts["child_cpu_ns"] - parts["sync_cpu_ns"]
        if own_cpu < 0:
            raise ValueError("Child and synchronization CPU exceed measured input CPU")
        measured.append(
            dict(
                parent,
                child_calls=parts["child_calls"],
                outside_child_sync_calls=parts["sync_calls"],
                exclusive_service_cpu_us=own_cpu / 1000,
                exclusive_ranges_ns=parts["exclusive_ranges_ns"],
            )
        )
    return measured


def pair_input_cpu(light: list[dict], profiled: list[dict]) -> list[dict]:
    """Pair admitted input observations in the shared CPU budget format.

    bind_input_calls has established one call per request/rank in each capture.
    Compare the captures' work without repeating that input admission here.
    """

    def index(rows: list[dict]) -> dict[tuple, dict]:
        return {(row["identity"]["request_ids"][0], row["identity"]["tp_rank"]): row for row in rows}

    a, b = index(light), index(profiled)
    if a.keys() != b.keys():
        raise ValueError("Paired input CPU identities differ")
    rows = []
    for request, rank in sorted(b):
        full = b[(request, rank)]
        normal = a[(request, rank)]
        if any(full[key] != normal[key] for key in ("child_calls", "outside_child_sync_calls")):
            raise ValueError("Paired input CPU child/synchronization counts differ")
        rows.append(paired_service(normal, full))
    return rows
