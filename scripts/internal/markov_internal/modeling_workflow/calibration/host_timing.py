"""Observe and pair same-base HiCache host CPU with explicit source ownership."""

from collections import defaultdict
from collections.abc import Iterable

from .cpu_scopes import contains, exclusive_cpu_parts, paired_service


def observe_host_cpu(steps: dict, report: dict, records: list[dict], syncs: list[dict]) -> dict:
    """Observe host work using steps already admitted by bind_forward_steps."""
    formal = report["formal_window"]
    lo, hi = [round(formal[key] * 1_000_000) for key in ("formal_begin_ms", "formal_end_ms")]
    outer = [row for row in records if "identity" in row]

    formal_requests = {row["request_id"] for row in steps["rows"]}

    def belongs_to_formal_request(row):
        return any(
            contains(parent, row)
            and parent["identity"].get("request_ids")
            and set(parent["identity"]["request_ids"]) <= formal_requests
            for parent in outer
        )

    hosts = [
        row
        for row in records
        if row["name"].startswith("hicache.host.")
        and (row["start_ns"] < hi and row["end_ns"] > lo or belongs_to_formal_request(row))
    ]
    if not hosts:
        raise ValueError("No measured HiCache host methods in the formal window")
    parents = {}
    for i, row in enumerate(hosts):
        containers = [j for j, candidate in enumerate(hosts) if i != j and contains(candidate, row)]
        if containers:
            parents[i] = min(containers, key=lambda j: hosts[j]["end_ns"] - hosts[j]["start_ns"])
    rows, unbound, partial, outside_http = [], [], [], []
    for i, row in enumerate(hosts):
        if not row["returned"]:
            raise ValueError("Host method did not return")
        children = [hosts[j] for j, parent in parents.items() if parent == i]
        parts = exclusive_cpu_parts(row, children, syncs)
        exclusive_cpu = row["thread_cpu_ns"] / 1000 - parts["child_cpu_ns"] / 1000
        if exclusive_cpu < 0:
            raise ValueError("Host children exceed parent measurements")
        containers = [r for r in outer if contains(r, row)]
        result = dict(
            row,
            exclusive_ranges_ns=parts["exclusive_ranges_ns"],
            child_calls=parts["child_calls"],
            parent_host=hosts[parents[i]]["name"] if i in parents else None,
        )
        sync_cpu = parts["sync_cpu_ns"] / 1000
        if not 0 <= sync_cpu <= exclusive_cpu:
            raise ValueError("Synchronization exceeds exclusive host CPU")
        result["exclusive_service_cpu_us"] = exclusive_cpu - sync_cpu
        if containers:
            parent = min(containers, key=lambda r: r["end_ns"] - r["start_ns"])
            result["outer_scope"] = parent["name"]
            result["identity"] = parent["identity"]
        if row["end_ns"] <= lo or row["start_ns"] >= hi:
            outside_http.append(result)
            continue
        if row["start_ns"] < lo or row["end_ns"] > hi:
            partial.append(result)
            continue  # Never prorate partial thread CPU.
        if not containers:
            unbound.append(result)
            continue
        rows.append(result)
    return dict(
        scope="Host CPU envelopes; exclusive subtracts only immediate host children, not waits or recorder overhead",
        source_manifest=steps["source_manifest"],
        http_us=formal["e2e_ms"] * 1000,
        rows=rows,
        unbound=unbound,
        partial=partial,
        outside_http=outside_http,
    )


FACT_NAMES = {
    "hicache.host.start_loading": "hicache_loadback_io_observed_end",
    "hicache.host.load_back": "hicache_loadback_decision_observed_end",
    "hicache.host.write_backup": "hicache_commit_device_to_host_enqueue_observed_end",
    "hicache.host.prefill_admission": "hicache_prefill_admission_observed_end",
}


def _index_calls(document: dict) -> dict[tuple, dict]:
    if document.get("unbound") or document.get("partial") or not document["rows"]:
        raise ValueError("Host pairing requires complete, bound calls")
    result = {}
    ordinals = defaultdict(int)
    for row in sorted(document["rows"], key=lambda r: r["start_ns"]):
        ids = row["identity"]["request_ids"]
        if len(ids) != 1:
            raise ValueError("Host pairing requires explicit single-request identity")
        group = row["identity"]["tp_rank"], ids[0], row["name"], row["parent_host"] or ""
        key = (*group, ordinals[group])
        ordinals[group] += 1
        result[key] = row
    return result


def pair_host_cpu(light: dict, profiled: dict, probe_events: Iterable[dict]) -> dict:
    """Pair observed source intervals; the combined measurement set checks overlap."""

    # A method crossing HTTP completion has no measured in-window CPU budget.
    # Keep that source cost unchanged; never prorate it or reject unrelated calls.
    def with_boundaries(document: dict) -> dict[tuple, dict]:
        boundary = document.get("partial", []) + document.get("outside_http", [])
        if any("identity" not in row for row in boundary):
            raise ValueError("Boundary host method has no explicit request identity")
        return _index_calls(dict(document, rows=document["rows"] + boundary, partial=[]))

    a, b = with_boundaries(light), with_boundaries(profiled)
    if a.keys() != b.keys():
        raise ValueError("Paired host call identities differ")

    # Partition probe candidates once. Containment still decides ownership;
    # this index must not turn an ambiguous interval into an arbitrary match.
    facts = defaultdict(list)
    for ordinal, event in enumerate(probe_events):
        if event.get("name") in FACT_NAMES.values():
            facts[(event["name"], str(event["pid"]), str(event["tid"]))].append((ordinal, event))

    outside_profiled = {id(row) for row in profiled.get("outside_http", [])}
    partial_profiled = {id(row) for row in profiled.get("partial", [])}
    boundary_light = {id(row) for row in light.get("partial", []) + light.get("outside_http", [])}
    rows, used = [], set()
    skipped, recovered = [], []
    for key, call in b.items():
        if id(call) in outside_profiled:
            continue  # No source geometry inside the formal window to correct.
        if id(call) in partial_profiled:
            skipped.append(key)
            continue
        if id(a[key]) in boundary_light:
            recovered.append(key)  # Whole measured reference cost, never clipped CPU.

        candidates = facts[(FACT_NAMES[call["name"]], str(call["pid"]), str(call["tid"]))]
        matches = [
            (i, e)
            for i, e in candidates
            if e["ts"] <= call["start_ns"] // 1000 and e["ts"] + e["dur"] >= call["end_ns"] // 1000
        ]
        if len(matches) != 1:
            raise ValueError("Host method must match exactly one source probe")
        i, fact = matches[0]
        if i in used or fact["args"]["status"] != "completed":
            raise ValueError("Source probe was reused or did not complete")
        used.add(i)
        if a[key]["child_calls"] != call["child_calls"]:
            raise ValueError("Same-base host nesting changed")

        row = paired_service(a[key], call)
        row.update(
            parent_host=key[3],
            ordinal=key[4],
            source_probe_begin_us=fact["ts"],
            source_probe_end_us=fact["ts"] + fact["dur"],
            operation_id=fact["args"].get("operation_id"),
        )
        rows.append(row)

    result = dict(
        scope="Development base-call binding, not transferable coefficients; probe wrapper CPU not measured by method timer",
        light_manifest=light["source_manifest"],
        source_manifest=profiled["source_manifest"],
        rows=rows,
    )
    if skipped:
        result["uncorrected_http_boundary_keys"] = skipped
    if recovered:
        result["light_boundary_keys"] = recovered
    return result
