"""Partition independent operation observations; never import target topology."""

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
