"""Measure scheduler CPU while retaining selection/run/result forward semantics."""

from .cpu_scopes import exclusive_cpu_parts, paired_service


def observe_scheduler_cpu(
    steps: dict,
    hosts: dict,
    report: dict,
    timers: list[dict],
    syncs: list[dict],
    *,
    measure_boundary_results: bool = False,
) -> dict:
    """Observe raw scheduler timers using steps and hosts from the same capture."""

    if hosts["unbound"]:
        raise ValueError("Scheduler CPU extraction needs bound host observations")
    window = report["formal_window"]
    lo, hi = [round(window[key] * 1_000_000) for key in ("formal_begin_ms", "formal_end_ms")]
    rows, partial, outside_http, boundary_rows = [], [], [], []
    formal_requests = {r["request_id"] for r in steps["rows"]}
    consumed_results = set()
    for scope in sorted(timers, key=lambda row: row["start_ns"]):
        begin, end = scope["start_ns"], scope["end_ns"]
        # Input processing has its own request-level audit, not a forward phase.
        if scope["name"] not in (
            "scheduler.get_next_batch_to_run",
            "scheduler.run_batch",
            "scheduler.process_batch_result",
        ):
            continue
        boundary = begin < lo or end > hi or end <= lo or begin >= hi
        if end <= lo or begin >= hi:
            if set(scope.get("identity", {}).get("request_ids", [])) & formal_requests:
                outside_http.append(scope)
        elif begin < lo or end > hi:
            partial.append(scope)
        if boundary and not (
            measure_boundary_results
            and scope["name"] in ("scheduler.run_batch", "scheduler.process_batch_result")
            and scope["returned"]
            and scope["identity"]["request_ids"]
            and set(scope["identity"]["request_ids"]) <= formal_requests
        ):
            continue
        lane = (scope["pid"], scope["tid"])
        if not boundary and any(
            (r["pid"], r["tid"]) == lane and r["start_ns"] < end and r["end_ns"] > begin for r in hosts["partial"]
        ):
            raise ValueError("Boundary host method crosses an in-window scheduler scope")
        # Cost observations may extend past the HTTP window. Use raw child scopes there,
        # not the window-filtered host list, and never prorate measured CPU.
        children = (
            [r for r in timers if r["name"].startswith(("step[", "hicache.host."))]
            if boundary
            else steps["rows"] + hosts["rows"]
        )
        parts = exclusive_cpu_parts(scope, children, syncs)
        child_cpu = parts["child_cpu_ns"] / 1000
        sync_cpu = parts["sync_cpu_ns"] / 1000
        own_cpu = scope["thread_cpu_ns"] / 1000 - child_cpu - sync_cpu
        if own_cpu < 0:
            raise ValueError("Measured children and synchronization exceed scheduler CPU")
        identity = scope["identity"]
        candidates = [
            s
            for s in steps["rows"]
            if (s["pid"], s["tid"]) == lane and s["identity"]["request_ids"] == identity["request_ids"]
        ]
        if scope["name"] == "scheduler.get_next_batch_to_run":
            following = [s for s in candidates if s["start_ns"] >= end]
            if identity["selected_request_ids"]:
                if identity["selected_request_ids"] != identity["request_ids"] or not following:
                    raise ValueError("Scheduler selection has no matching following forward")
            forward = min(following, key=lambda s: s["start_ns"]) if identity["selected_request_ids"] else None
        elif scope["name"] == "scheduler.run_batch":
            contained = [s for s in candidates if begin <= s["start_ns"] and s["end_ns"] <= end]
            if len(contained) != 1:
                raise ValueError("Scheduler run must contain exactly one measured forward")
            forward = contained[0]
        else:
            expected_phase = {"1": "step[EXTEND", "2": "step[DECODE"}[identity["forward_mode"]]
            preceding = [
                s
                for s in candidates
                if s["end_ns"] <= begin
                and s["name"].split(" ", 1)[0] == expected_phase
                and (s["pid"], s["tid"], s["start_ns"]) not in consumed_results
            ]
            if not preceding:
                raise ValueError("Scheduler result has no matching preceding forward")
            # SGLang appends run_batch results and consumes them with popleft;
            # overlap may launch another forward before processing this result.
            forward = min(preceding, key=lambda s: s["start_ns"])
            consumed_results.add((forward["pid"], forward["tid"], forward["start_ns"]))
        phase = forward["name"].split(" ", 1)[0] if forward else "empty"
        if "forward_mode" in identity:
            # Captured IntEnum strings: ForwardMode.EXTEND=1, DECODE=2 in this source.
            expected_mode = {"step[EXTEND": "1", "step[DECODE": "2"}[phase]
            if identity["forward_mode"] != expected_mode:
                raise ValueError("Scheduler and forward mode disagree")
        (boundary_rows if boundary else rows).append(
            dict(
                scope,
                child_calls=parts["child_calls"],
                phase=phase,
                forward_start_ns=forward["start_ns"] if forward else None,
                forward_ordinal=forward.get("forward_ordinal", 0) if forward else None,
                exclusive_ranges_ns=parts["exclusive_ranges_ns"],
                outside_child_sync_calls=parts["sync_calls"],
                exclusive_service_cpu_us=own_cpu,
            )
        )
    bindings = [
        (r["pid"], r["tid"], r["name"], r["forward_start_ns"])
        for r in rows + boundary_rows
        if r["forward_start_ns"] is not None
    ]
    if len(bindings) != len(set(bindings)):
        raise ValueError("Scheduler calls reuse one forward identity")
    result = dict(
        scope="Same-base diagnostic only; no target fit or partial CPU apportionment",
        source_manifest=steps["source_manifest"],
        rows=rows,
        partial=partial,
        outside_http=outside_http,
    )
    if measure_boundary_results:
        result["boundary_observations"] = boundary_rows
    return result


def _index(document):
    result = {}
    for row in document["rows"]:
        if row["phase"] == "empty":
            continue
        identity = row["identity"]
        if len(identity["request_ids"]) != 1:
            raise ValueError("Scheduler service requires a single explicit request")
        key = (identity["tp_rank"], identity["request_ids"][0], row["name"], row["phase"], row["forward_ordinal"])
        if key in result:
            raise ValueError("Scheduler calls reuse one forward ordinal")
        result[key] = row
    return result


def pair_scheduler_cpu(light: dict, profiled: dict) -> dict:
    before, after = _index(light), _index(profiled)
    boundary = _index({"rows": light.get("boundary_observations", [])})
    if boundary.keys() & before.keys():
        raise ValueError("Boundary and in-window scheduler calls reuse one identity")
    recovered = sorted((after.keys() - before.keys()) & boundary.keys())
    before.update((key, boundary[key]) for key in recovered)
    partial_keys = set()
    for row in profiled["partial"] + profiled["outside_http"]:
        if row["name"] not in ("scheduler.run_batch", "scheduler.process_batch_result"):
            continue
        identity = row["identity"]
        if len(identity["request_ids"]) != 1:
            raise ValueError("Scheduler tail requires an explicit request")
        phase = {"1": "step[EXTEND", "2": "step[DECODE"}[identity["forward_mode"]]
        partial_keys.add((identity["tp_rank"], identity["request_ids"][0], row["name"], phase))
    if after.keys() - before.keys() or not {key[:4] for key in before.keys() - after.keys()} <= partial_keys:
        raise ValueError("Scheduler pairs differ without measured HTTP boundary evidence")
    rows = []
    for key in sorted(after):
        old, new = before[key], after[key]
        if (old["child_calls"], old["outside_child_sync_calls"]) != (
            new["child_calls"],
            new["outside_child_sync_calls"],
        ):
            raise ValueError("Scheduler pair has different measured child or sync work")
        rows.append(dict(paired_service(old, new), phase=key[3], forward_ordinal=key[4]))
    result = dict(
        scope="Measured same-base scheduler service; unmatched tail and empty selections not normalized",
        source_manifest=profiled["source_manifest"],
        light_manifest=light["source_manifest"],
        unmatched_light_keys=sorted(before.keys() - after.keys()),
        rows=rows,
    )
    if recovered:
        result["light_boundary_keys"] = recovered
    return result
