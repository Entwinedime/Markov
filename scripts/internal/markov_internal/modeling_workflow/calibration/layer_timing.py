"""Same-base layer host CPU measurements, sharing the enclosing forward budget."""

from .cpu_scopes import overlaps


def observe_layer_cpu(steps: list[dict], events: list[dict], synchronizations: list[dict]) -> dict[tuple, dict]:
    """Observe raw layer timers within uniquely numbered, admitted forward steps."""

    batches = [e for e in events if e["name"] == "hicache.layer_wait_batch"]
    rows = {}
    for step in sorted(steps, key=lambda row: row["start_ns"]):
        identity = step["identity"]
        matches = [
            e
            for e in batches
            if (e["pid"], e["tid"]) == (step["pid"], step["tid"])
            and e["identity"]["request_ids"] == [step["request_id"]]
            and e["identity"]["tp_rank"] == identity["tp_rank"]
            and e["start_ns"] <= step["start_ns"]
            and e["end_ns"] >= step["end_ns"]
        ]
        if len(matches) != 1 or not matches[0]["returned"]:
            raise ValueError("Forward requires one complete layer batch")
        calls = matches[0]["identity"]["calls"]
        if not calls:
            raise ValueError("Layer batch is empty")

        sync = [e for e in synchronizations if overlaps(e, step)]
        end = step["start_ns"]
        for call in calls:
            if (
                not call["returned"]
                or call["start_ns"] < end
                or call["end_ns"] < call["start_ns"]
                or call["end_ns"] > step["end_ns"]
                or type(call["thread_cpu_ns"]) is not int
                or call["thread_cpu_ns"] < 0
            ):
                raise ValueError("Invalid or overlapping layer timing")
            if any(e["start_ns"] < call["end_ns"] and e["end_ns"] > call["start_ns"] for e in sync):
                raise ValueError("Layer CPU overlaps separately accounted synchronization")
            end = call["end_ns"]

        cpu = sum(c["thread_cpu_ns"] for c in calls)
        if cpu > step["thread_cpu_ns"]:
            raise ValueError("Layer CPU exceeds enclosing forward CPU")
        key = (step["request_id"], identity["tp_rank"], step["name"].split(" ", 1)[0], step.get("forward_ordinal", 0))
        rows[key] = dict(pid=step["pid"], tid=step["tid"], calls=calls, cpu_us=cpu / 1000)
    return rows


def pair_layer_cpu(
    light: dict[tuple, dict], profiled: dict[tuple, dict], comparison: dict, source_manifest: str
) -> dict:
    """Reserve signed integer layer budgets; leave the measured total unchanged."""
    budgets = {(r["request_id"], r["rank"], r["phase"], r.get("forward_ordinal", 0)): r for r in comparison["rows"]}
    if light.keys() != profiled.keys() or profiled.keys() != budgets.keys():
        raise ValueError("Layer and forward measurements have different identities")

    rows = []
    for key, full in profiled.items():
        normal = light[key]
        layer_calls = [(call["layer"], call["active"]) for call in full["calls"]]
        if [(call["layer"], call["active"]) for call in normal["calls"]] != layer_calls:
            raise ValueError("Paired layer work or active state differs")

        delta = full["cpu_us"] - normal["cpu_us"]
        reserved = round(delta)
        # Sub-microsecond intervals carry no representable graph width.
        ranges = [[c["start_ns"], c["end_ns"]] for c in full["calls"] if c["end_ns"] // 1000 > c["start_ns"] // 1000]
        if not ranges and reserved:
            raise ValueError("Layer budget has no representable source interval")
        budgets[key]["layer_reduction_us"] = reserved
        if ranges:
            rows.append(
                dict(
                    request_id=key[0],
                    rank=key[1],
                    phase=key[2],
                    forward_ordinal=key[3],
                    pid=full["pid"],
                    tid=full["tid"],
                    method="hicache.layer_wait",
                    operation_id=None,
                    exclusive_ranges_ns=ranges,
                    layer_calls=layer_calls,
                    normal_service_cpu_us=normal["cpu_us"],
                    profiled_service_cpu_us=full["cpu_us"],
                    measured_service_delta_us=reserved,
                    unrounded_delta_us=delta,
                )
            )
    return dict(source_manifest=source_manifest, rows=rows)
