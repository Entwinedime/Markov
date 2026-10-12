"""Partition independent operation observations; never import target topology."""

from typing import Any

ALLOCATION_STAGES = [
    ("multiply", "Enqueue@aclnnMuls"),
    ("range", "Enqueue@aclnnArange"),
    ("add", "Enqueue@aclnnAdd"),
]
CLONE_SUBMISSION = "Enqueue@aclnnInplaceCopy"


def _submitted_work(row: dict, submission: int) -> tuple[dict, dict]:
    """Resolve one measured single-launch task; keep launch and return separate."""
    nodes = row["nodes"]
    jobs = [node for node in nodes if node.get("submission") == submission]
    heads = [node for node in jobs if "task_predecessor" not in node]
    if len(heads) != 1:
        raise ValueError("Index cost model requires one complete worker task")

    worker = heads[0]
    members = {node["id"] for node in jobs}
    devices = {end for start, end, _ in row["edges"] if start in members and not nodes[end]["cpu"]}
    if len(devices) != 1:
        raise ValueError("Worker must identify one device operation")

    costs = row["worker_costs"].get(str(submission))
    if costs is None:
        raise ValueError("Worker launch/return intervals are not independently measurable")
    return dict(worker, **costs), nodes[next(iter(devices))]


def index_costs(row: dict[str, Any]) -> dict[str, Any]:
    if row["device_streams"] != 1 or row["stream_waits"] or row["event_waits"]:
        raise ValueError("Index costs require a complete asynchronous single-compute-stream observation")

    nodes = row["nodes"]
    main = [node for node in nodes if node["cpu"] and not node["worker"]]
    calls = [node for node in main if node["name"].startswith("Enqueue@")]
    if [node["name"] for node in calls[:3]] != [name for _, name in ALLOCATION_STAGES]:
        raise ValueError("Independent load does not contain the supported allocation primitives")

    clones = calls[3:]
    if not clones or any(node["name"] != CLONE_SUBMISSION for node in clones):
        raise ValueError("Allocation must be followed by the observed promoted-node clones")
    # One clone necessarily covers the full successful allocation. Multiple
    # clones need measured per-node lengths; never divide by the call count.
    clone_tokens = row.get("clone_tokens", [row["tokens"]] if len(clones) == 1 else None)
    if clone_tokens is None or len(clone_tokens) != len(clones) or sum(clone_tokens) != row["tokens"]:
        raise ValueError("Promoted-node clone lengths are missing or do not match the allocation")
    stages = [(kind, name, row["tokens"]) for kind, name in ALLOCATION_STAGES]
    stages.extend(("clone", CLONE_SUBMISSION, tokens) for tokens in clone_tokens)

    result, previous, assigned = [], -1, set()
    for (kind, name, tokens), call in zip(stages, calls):
        parts = [node for node in main if previous < node["id"] <= call["id"]]
        worker, device = _submitted_work(row, call["id"])
        if not worker["worker"] or device["id"] in assigned:
            raise ValueError("Index submission requires a proven worker and uniquely owned device operation")

        assigned.add(device["id"])
        result.append(
            dict(
                operation=kind,
                requested_tokens=tokens,
                submission_name=name,
                main_us=sum(n["service_us"] for n in parts),
                main_residual_us=sum(n["residual_us"] for n in parts),
                worker_us=worker["worker_us"],
                worker_residual_us=worker["worker_residual_us"],
                worker_return_us=worker["worker_return_us"],
                worker_return_residual_us=worker["worker_return_residual_us"],
                dispatch_us=worker["dispatch_us"],
                device_us=device["service_us"],
            )
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


def index_observations(row: dict[str, Any], sample: dict[str, Any], page_size: int) -> list[dict[str, Any]]:
    """Retain each measured call's work point, rather than a per-rank mean."""
    tokens = row["tokens"]
    if tokens % page_size:
        raise ValueError("Unsorted paged index measurements require whole token pages")

    position = f"load_index:{row['pid']}:{row['tid']}:{row['begin_us']}"
    observations: list[dict[str, Any]] = []
    scopes = {
        "main_us": "cpu_service",
        "main_residual_us": "residual_interval",
        "worker_us": "worker_launch_service",
        "worker_residual_us": "worker_launch_residual_interval",
        "worker_return_us": "worker_return_service",
        "worker_return_residual_us": "worker_return_residual_interval",
        "dispatch_us": "dispatch_interval",
        "device_us": "device_service",
    }
    for index, costs in enumerate(sample["operations"]):
        operation = costs["operation"]
        operation_tokens = costs["requested_tokens"]
        if operation_tokens <= 0 or operation_tokens % page_size:
            raise ValueError("Index operation measurements require nonempty whole token pages")
        features = {
            "requested_tokens": operation_tokens,
            "page_size_tokens": page_size,
            "pages": operation_tokens // page_size,
        }
        if operation == "clone":
            features["first_clone"] = int(index == len(ALLOCATION_STAGES))
        for field, scope in scopes.items():
            observations.append(
                dict(
                    position=f"{position}:{operation}:{index}",
                    operation=f"load.index.{operation}",
                    implementation="ascend.paged_unsorted",
                    features=features,
                    duration_us=costs[field],
                    timing_scope=scope,
                )
            )

    features = {"requested_tokens": tokens, "page_size_tokens": page_size, "pages": tokens // page_size}
    for field, scope in (("tail_us", "cpu_service"), ("tail_residual_us", "residual_interval")):
        observations.append(
            dict(
                position=f"{position}:tail",
                operation="load.index.tail",
                implementation="ascend.paged_unsorted",
                features=features,
                duration_us=sample[field],
                timing_scope=scope,
            )
        )
    return observations
