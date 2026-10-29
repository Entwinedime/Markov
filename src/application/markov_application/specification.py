"""Validate the public exploration input before starting native work."""

import json
import re
from pathlib import Path
from typing import Any

from markov_collection.adapters.frameworks.sglang.acquisition import SGLangAcquisition


def check_fields(source: dict[str, Any], fields: set[str], context: str) -> None:
    unknown = source.keys() - fields
    if unknown:
        raise ValueError(f"unsupported {context} fields: {sorted(unknown)}")


def read_specification(path: Path) -> dict[str, Any]:
    document = json.loads(path.read_text())
    check_fields(
        document,
        {"framework", "model", "deployment", "budget", "configurations", "workloads", "node_durations_ns"},
        "task",
    )
    check_fields(document["model"], {"path", "dtype"}, "model")
    check_fields(document["deployment"], {"physical_devices"}, "deployment")
    check_fields(
        document["budget"], {"native_seconds", "processing_seconds", "temporary_bytes", "retained_bytes"}, "budget"
    )
    if document["framework"] != "sglang":
        raise ValueError("this acquisition entrypoint currently provides SGLang/Ascend")
    deployment = document["deployment"]["physical_devices"]
    if (
        not isinstance(deployment, list)
        or not deployment
        or any(type(device) is not int or device < 0 for device in deployment)
        or len(set(deployment)) != len(deployment)
    ):
        raise ValueError("physical_devices must contain distinct nonnegative integers")
    for category in ("configurations", "workloads"):
        identities = [item["id"] for item in document[category]]
        if (
            not identities
            or len(set(identities)) != len(identities)
            or any(not re.fullmatch(r"[A-Za-z0-9_-]+", name) for name in identities)
        ):
            raise ValueError(f"{category} need nonempty, unique path-safe IDs")
    maximum = (1 << 63) - 1
    topologies = {}
    for configuration in document["configurations"]:
        pd = "pd" in configuration
        capacity = {"max_running_requests", "max_total_tokens", "max_prefill_tokens"}
        geometry = {"tp", "pp", "ep"}
        check_fields(
            configuration,
            {"id", "attention_backend", "page_size", "placement"}
            | ({"pd"} if pd else geometry | capacity | {"microbatch_requests"}),
            "configuration",
        )
        if type(configuration["page_size"]) is not int or not 0 < configuration["page_size"] <= maximum:
            raise ValueError("configuration page_size is outside its positive integer range")

        pools = [(configuration, capacity)]
        if pd:
            check_fields(configuration["pd"], {"prefill", "decode"}, "pd")
            pools = []
            for role in ("prefill", "decode"):
                fields = capacity if role == "prefill" else capacity - {"max_prefill_tokens"}
                pool = configuration["pd"][role]
                check_fields(pool, fields | geometry, "pd." + role)
                pools.append((pool, fields))

        ranks = 0
        for pool, fields in pools:
            for name in fields | {"tp"}:
                if type(pool[name]) is not int or not 0 < pool[name] <= maximum:
                    raise ValueError(f"configuration {name} is outside its positive integer range")
            if pool["max_total_tokens"] < configuration["page_size"]:
                raise ValueError("configuration KV capacity needs at least one page")
            pp = pool.get("pp", 1)
            if type(pp) is not int or not 0 < pp <= maximum:
                raise ValueError("configuration pp is outside its positive integer range")
            ranks += pool["tp"] * pp

            if "ep" in pool:
                ep = pool["ep"]
                check_fields(ep, {"id", "size"}, "ep")
                if (
                    not isinstance(ep["id"], str)
                    or not ep["id"]
                    or type(ep["size"]) is not int
                    or not 0 < ep["size"] <= maximum
                ):
                    raise ValueError("EP topology needs a nonempty ID and positive integer size")
                layout = pool["tp"], pp, ep["size"]
                if ep["id"] in topologies and topologies[ep["id"]] != layout:
                    raise ValueError("one EP topology ID cannot refer to different parallel layouts")
                topologies[ep["id"]] = layout

        if not pd:
            microbatch = configuration.get("microbatch_requests", (configuration["max_running_requests"] - 1) // pp + 1)
            if type(microbatch) is not int or not 0 < microbatch <= configuration["max_running_requests"]:
                raise ValueError("microbatch_requests must fit the request capacity")
        domain = SGLangAcquisition.domain(document["model"], configuration)
        if SGLangAcquisition.required_devices(domain) > len(deployment):
            raise ValueError("declared collection devices cannot run the required native source participants")

        if "placement" in configuration:
            placement = configuration["placement"]
            check_fields(placement, {"devices"}, "placement")
            devices = placement["devices"]
            if (
                not isinstance(devices, list)
                or len(devices) != ranks
                or any(type(device) is not int or not 0 <= device <= maximum for device in devices)
                or len(set(devices)) != len(devices)
            ):
                raise ValueError("placement needs one distinct nonnegative device per logical rank")

    # Reject malformed workloads before paying for a native source; per-configuration capacity remains a result.
    for workload in document["workloads"]:
        check_fields(workload, {"id", "requests", "ep_routes"}, "workload")
        routes = workload.get("ep_routes", {})
        if not isinstance(routes, dict) or routes.keys() - topologies.keys():
            raise ValueError("workload routes must be keyed by declared EP topology IDs")
        requests = workload["requests"]
        identities = [request["id"] for request in requests]
        if (
            not requests
            or any(not isinstance(name, str) or not name for name in identities)
            or len(set(identities)) != len(identities)
        ):
            raise ValueError("workload request IDs must be nonempty strings and unique")
        for request in requests:
            check_fields(request, {"id", "arrival_ns", "prompt_tokens", "output_tokens"}, "request")
            for name in ("arrival_ns", "prompt_tokens", "output_tokens"):
                value = request[name]
                minimum = 0 if name == "arrival_ns" else 1
                if type(value) is not int or not minimum <= value <= maximum:
                    raise ValueError(f"request {name} is outside its integer range: {request['id']}")
            if request["prompt_tokens"] + request["output_tokens"] - 1 > maximum:
                raise ValueError(f"request token length overflows: {request['id']}")

    durations = document["node_durations_ns"]
    if not isinstance(durations, dict) or any(
        type(value) is not int or not 0 <= value <= maximum for value in durations.values()
    ):
        raise ValueError("given node durations must contain nonnegative integer nanoseconds")

    for name in ("native_seconds", "processing_seconds", "temporary_bytes", "retained_bytes"):
        if type(document["budget"][name]) is not int or document["budget"][name] < 0:
            raise ValueError(f"budget {name} must be a nonnegative integer")
    return document
