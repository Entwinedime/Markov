"""Partition independent operation observations; never import target topology."""

from statistics import mean

STAGES = [
    ("multiply", "Enqueue@aclnnMuls"),
    ("range", "Enqueue@aclnnArange"),
    ("add", "Enqueue@aclnnAdd"),
    ("clone", "Enqueue@aclnnInplaceCopy"),
]


def _submitted_work(row: dict, submission: int) -> tuple[dict, dict]:
    """Resolve one observed worker/device pair; node ids are local array offsets."""
    nodes = row["nodes"]
    jobs = [node for node in nodes if node.get("submission") == submission]
    if len(jobs) != 1:
        raise ValueError("Submission must identify one worker task")

    worker = jobs[0]
    devices = {end for start, end, _ in row["edges"] if start == worker["id"] and not nodes[end]["cpu"]}
    if len(devices) != 1:
        raise ValueError("Worker must identify one device operation")

    return worker, nodes[next(iter(devices))]


def index_costs(row: dict) -> dict:
    if row["device_streams"] != 1 or row["stream_waits"] or row["event_waits"]:
        raise ValueError("Index costs require a complete asynchronous single-compute-stream observation")

    nodes = row["nodes"]
    main = [node for node in nodes if node["cpu"] and not node["worker"]]
    calls = [node for node in main if node["name"].startswith("Enqueue@")]
    if [node["name"] for node in calls] != [name for _, name in STAGES]:
        raise ValueError("Independent load does not contain exactly the supported index primitives")

    result, previous, assigned = {}, -1, set()
    for (kind, name), call in zip(STAGES, calls):
        parts = [node for node in main if previous < node["id"] <= call["id"]]
        worker, device = _submitted_work(row, call["id"])
        if not worker["worker"] or device["id"] in assigned:
            raise ValueError("Index submission requires a proven worker and uniquely owned device operation")

        assigned.add(device["id"])
        result[kind] = dict(
            submission_name=name,
            main_us=sum(n["service_us"] for n in parts),
            main_residual_us=sum(n["residual_us"] for n in parts),
            worker_us=worker["service_us"],
            dispatch_us=worker["dispatch_us"],
            device_us=device["service_us"],
        )
        previous = call["id"]

    if assigned != {node["id"] for node in nodes if not node["cpu"]}:
        raise ValueError("Unassigned device work in independent load")

    tail = [node for node in main if node["id"] > previous]
    return dict(
        operations=result,
        tail_us=sum(n["service_us"] for n in tail),
        tail_residual_us=sum(n["residual_us"] for n in tail),
    )


def submission_costs(row: dict) -> dict:
    if row["event_waits"] or len(row["waits"]) != 1:
        raise ValueError("Expected complete Ascend load with one index synchronization")

    nodes = row["nodes"]
    main = [n for n in nodes if n["cpu"] and "submission" not in n]
    sync = row["waits"][0]["consumer"]
    records = [n["id"] for n in main if n["name"].endswith("Enqueue@record_event")]
    waits = [n["id"] for n in main if n["name"].endswith("Enqueue@wait_event")]
    copies = [n["id"] for n in main if n["name"].endswith("AscendCL@aclrtMemcpy2dAsync")]
    if len(waits) != 1 or len(records) != len(row["records"]) + 1 or not copies or len(copies) % 2:
        raise ValueError("Unexpected load event or K/V submission count")
    if not sync < records[0] < waits[0] < copies[0] <= copies[-1] < records[1]:
        raise ValueError("Expected index sync, compute record, load wait, all-layer copies, then layer records")

    previous = -1

    def segment(end: int, worker: bool = False) -> dict[str, float]:
        nonlocal previous
        parts = [n for n in main if previous < n["id"] <= end]
        result = dict(
            main_us=sum(n["service_us"] for n in parts),
            main_residual_us=sum(n["residual_us"] for n in parts),
            worker_us=0,
            dispatch_us=0,
            device_us=0,
        )
        if worker:
            job, device = _submitted_work(row, end)
            result.update(worker_us=job["service_us"], dispatch_us=job["dispatch_us"], device_us=device["service_us"])

        previous = end
        return result

    result = dict(before_sync=segment(sync), start_record=segment(records[0], True), wait_event=segment(waits[0], True))
    result["first_copy"] = segment(copies[0])
    steady_copies = [segment(node) for node in copies[1:]]
    result["copy"] = {key: mean(x[key] for x in steady_copies) for key in result["first_copy"]}

    result["first_layer_record"] = segment(records[1], True)
    steady_records = [segment(node, True) for node in records[2:]]
    result["layer_record"] = {key: mean(x[key] for x in steady_records) for key in result["first_layer_record"]}
    result["tail"] = segment(row["host_return"])

    # CPU work and residuals must be partitioned exactly before averaging.
    for field, source in [("main_us", "service_us"), ("main_residual_us", "residual_us")]:
        total = sum(
            result[k][field]
            for k in ["before_sync", "start_record", "wait_event", "first_copy", "first_layer_record", "tail"]
        )
        total += sum(x[field] for x in steady_copies + steady_records)
        if total != sum(n[source] for n in main):
            raise ValueError("Unassigned main-thread load cost")

    return result
