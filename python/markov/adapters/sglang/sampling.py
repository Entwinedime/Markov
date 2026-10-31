"""Turn shared operation-domain gaps into small independent SGLang measurements."""

from dataclasses import asdict
import math
import json
from pathlib import Path

from markov.artifacts.capture import DeviceCommand
from markov.artifacts.request import Deployment
from markov.adapters.sglang.capture import CAPTURE_OWNED_ARGUMENTS, capture_environment, server_arguments


def _region_capture_command(deployment: Deployment, configuration: dict, workload: dict, adapter: str) -> DeviceCommand:
    environment = capture_environment(False, True)
    payload = {
        "deployment": asdict(deployment),
        "server": server_arguments(deployment, configuration),
        "environment": environment,
        "profiling": False,
        "calibration": True,
        "workload": workload,
    }
    return DeviceCommand(
        "markov.adapters.sglang.capture",
        ("collect", "--input", "/capture/capture_input.json"),
        payload,
        "region_measurements.json",
        "calibration",
        ((Path(deployment.model), "/model", False),),
        environment,
        len(workload["requests"]),
        sum(len(row["input_ids"]) for row in workload["requests"]),
        sum(row["max_new_tokens"] for row in workload["requests"]),
        "workload_report.json",
        adapter,
    )


def region_workload(maximum: int, page: int, final_phases: list[str]) -> dict:
    """Fresh prompts separate reuse from size; one paired prefix varies attention work."""
    maximum = max(2, maximum)
    middle = max(1, maximum // 2)
    lengths = [maximum, maximum, 1, 1, middle, middle]
    requests = []

    def prompt(length: int, identity: int) -> list[int]:
        # Binary prefixes use ordinary vocabulary indices and avoid sharing a
        # complete page between the independent size points in the usual domain.
        bits = [int(bit) for bit in f"{identity:08b}"]
        return (bits + [0] * length)[:length]

    for identity, length in enumerate(lengths, 1):
        requests.append(
            {
                "prompt": None,
                "input_ids": prompt(length, identity),
                "max_new_tokens": 3,
                "ignore_eos": True,
                "return_logprob": False,
            }
        )
    prefix = (maximum // (2 * page)) * page
    if prefix:
        first = prompt(maximum, len(requests) + 1)
        second = first[:prefix] + [1] * (maximum - prefix)
        for tokens in (first, second):
            requests.append(
                {"prompt": None, "input_ids": tokens, "max_new_tokens": 3, "ignore_eos": True, "return_logprob": False}
            )
    if "prefill" in final_phases:
        for length in sorted({1, middle, maximum}):
            requests.append(
                {
                    "prompt": None,
                    "input_ids": prompt(length, len(requests) + 1),
                    "max_new_tokens": 1,
                    "ignore_eos": True,
                    "return_logprob": False,
                }
            )
        requests.append(
            {
                "prompt": None,
                "input_ids": prompt(maximum, len(requests) + 1),
                "max_new_tokens": 3,
                "ignore_eos": True,
                "return_logprob": False,
            }
        )
    return {"requests": requests, "formal_begin_index": 0}


def sampling_commands(deployment: Deployment, report: dict, requirements: list[dict]) -> list[DeviceCommand]:
    """Only work, supported implementation and source geometry determine recipes."""
    if (deployment.framework, deployment.runtime) != ("sglang", "ascend"):
        return []
    context = report["sampling_context"]
    cells = {cell["name"]: cell for cell in report["cells"]}
    regions = {}
    io_cases = set()
    io_operations = set()
    scheduler_pages = set()
    geometry = context["geometry"]
    for requirement in requirements:
        work = requirement.get("work")
        if work is None:
            continue
        operation = work["operation"]
        if operation == "scheduler.idle_turn":
            scheduler_pages.update(int(cells[name]["configuration"]["page_size"]) for name in requirement["targets"])
        elif operation.split(".", 1)[0] in ("prepare", "batch", "result", "result_send", "result_tail"):
            for name in requirement["targets"]:
                configuration = cells[name]["configuration"]
                regions[(int(configuration["page_size"]), bool(configuration["enable_hierarchical_cache"]))] = (
                    configuration
                )
        elif operation in (
            "cache.backup.caller",
            "cache.backup.completion",
            "cache.index_clone",
            "cache.load.prepare",
            "cache.load.caller",
            "cache.load.completion",
        ):
            io_operations.add("load" if operation.startswith("cache.load.") else "backup")
            for name in requirement["targets"]:
                page = int(cells[name]["configuration"]["page_size"])
                needed = (
                    int(work["features"]["pages"])
                    if "pages" in work["features"]
                    else math.ceil(work["features"]["elements"] / page)
                )
                segments = int(work["features"].get("segments", 1))
                io_cases.update((page, pages, 1) for pages in (1, max(2, needed)))
                if segments > 1:
                    io_cases.add((page, max(needed, segments), segments))
        elif operation.startswith(("cache.prefetch.", "cache.storage_write.")):
            io_operations.add("file")
            for name in requirement["targets"]:
                page = int(cells[name]["configuration"]["page_size"])
                io_cases.update((page, pages, 1) for pages in (1, max(2, int(work["features"]["pages"]))))

    commands = []
    for (page, enabled), target in sorted(regions.items()):
        workload = region_workload(math.ceil(context["max_context_tokens"]), page, context["final_phases"])
        capacity = sum(
            math.ceil((len(row["input_ids"]) + row["max_new_tokens"]) / page) * page for row in workload["requests"]
        )
        configuration = {key: value for key, value in target.items() if key not in CAPTURE_OWNED_ARGUMENTS}
        for key in (
            "hicache_storage_backend",
            "hicache_storage_prefetch_policy",
            "hicache_storage_backend_extra_config",
        ):
            configuration.pop(key, None)
        configuration.update(
            page_size=page,
            enable_hierarchical_cache=enabled,
            hicache_write_policy="write_back",
            hicache_ratio=1.0,
            max_total_tokens=capacity,
            max_prefill_tokens=math.ceil(context["max_context_tokens"] / page) * page,
        )
        commands.append(_region_capture_command(deployment, configuration, workload, "sglang_regions"))
    if scheduler_pages:
        page = min(scheduler_pages)
        # Two alternating prefixes exceed both small pools; later requests
        # exercise real pending file prefetch, rather than an artificial sleep.
        workload = {
            "requests": [
                {
                    "prompt": None,
                    "input_ids": [index % 2] * (7 * page + 1),
                    "max_new_tokens": 1,
                    "ignore_eos": True,
                    "return_logprob": False,
                }
                for index in range(4)
            ],
            "formal_begin_index": 0,
        }
        configuration = {
            "page_size": page,
            "max_total_tokens": 10 * page,
            "max_prefill_tokens": 8 * page,
            "enable_hierarchical_cache": True,
            "hicache_ratio": 1.1,
            "hicache_write_policy": "write_through",
            "hicache_storage_backend": "file",
            "hicache_storage_prefetch_policy": "wait_complete",
            "hicache_storage_backend_extra_config": json.dumps({"prefetch_threshold": page}),
        }
        commands.append(_region_capture_command(deployment, configuration, workload, "sglang_scheduler"))
    if io_cases:
        if geometry["key_dim"] != geometry["value_dim"]:
            raise ValueError("The current independent I/O sampler requires equal K/V head dimensions")
        recipe = {
            "device": 0,
            "geometry": {
                "layers": int(geometry["layers"]),
                "kv_heads": int(geometry["kv_heads"]),
                "head_dim": int(geometry["key_dim"]),
            },
            "warmup": 1,
            "repeats": 3,
            "operations": sorted(io_operations),
            "cases": [
                {"page_tokens": page, "pages": pages, "segments": segments}
                for page, pages, segments in sorted(io_cases)
            ],
        }
        commands.append(
            DeviceCommand(
                "markov.adapters.sglang.cache_io",
                ("--recipe", "/capture/capture_input.json", "--output", "/capture/data"),
                recipe,
                "data/capture.json",
                "cache_io",
                environment=capture_environment(False),
                measurement_adapter="sglang_cache_io",
            )
        )
    return commands
