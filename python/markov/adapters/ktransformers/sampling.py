"""Prepare shared, independent KT measurements for an entire target group."""

from dataclasses import asdict
from pathlib import Path

from markov.artifacts.capture import DeviceCommand
from markov.adapters.ktransformers.capture import auxiliary_mounts, capture_environment, server_arguments


def sampling_commands(deployment, report, requirements):
    features = [
        row["work"]["features"] for row in requirements if "work" in row and row["work"]["operation"].startswith("forward.")
    ]
    if not features:
        return []
    configurations = []
    keys = {"cpu_threads": "kt_cpuinfer", "numa_pools": "kt_threadpool_count", "resident_experts": "kt_num_gpu_experts"}
    # Share extrema across the group instead of sampling each target/workload cell.
    selected = []
    for key in keys:
        for choose in (min, max):
            point = choose(features, key=lambda row: row[key])
            counts = tuple(int(point[key]) for key in keys)
            if counts not in selected:
                selected.append(counts)
    for counts in selected:
        settings = {name: value for name, value in zip(keys.values(), counts)}
        configuration = {**report["sampling_context"]["configuration"], **settings}
        configurations.append(configuration)
    query = max(max(int(row["query_tokens"]) for row in features), int(report["sampling_context"]["max_query_tokens"]))
    lengths = sorted({1, max(2, query // 2), query})
    workload = {"formal_begin_index": 1, "drain_before_formal": False, "requests": []}
    for length in [lengths[0], *lengths]:
        workload["requests"].append(
            {
                "prompt": None,
                "input_ids": [1] + list(range(10, 10 + length - 1)),
                "max_new_tokens": 16,
                "ignore_eos": True,
                "return_logprob": False,
            }
        )
    # Equal lengths with different token patterns separate routing distribution
    # from query size; several Decode steps expose the post-Prefill transition.
    for tokens in (list(range(10, 10 + query - 1))[::-1], [10 + index % 32 for index in range(query - 1)]):
        workload["requests"].append(
            {
                "prompt": None,
                "input_ids": [1, *tokens],
                "max_new_tokens": 16,
                "ignore_eos": True,
                "return_logprob": False,
            }
        )
    commands = []
    for configuration in configurations:
        environment = capture_environment(False, True)
        payload = {
            "deployment": asdict(deployment),
            "configuration": configuration,
            "server": server_arguments(deployment, configuration, calibration=True),
            "environment": environment,
            "profiling": False,
            "calibration": True,
            "record_regions": False,
            "workload": workload,
        }
        commands.append(
            DeviceCommand(
                "markov.adapters.ktransformers.capture",
                ("collect", "--input", "/capture/capture_input.json"),
                payload,
                "region_measurements.json",
                "calibration",
                ((Path(deployment.model), "/model", False), *auxiliary_mounts(configuration)),
                environment,
                len(workload["requests"]),
                sum(len(row["input_ids"]) for row in workload["requests"]),
                sum(row["max_new_tokens"] for row in workload["requests"]),
                "workload_report.json",
                "ktransformers_regions",
                ("runtime.kt.batch", "runtime.kt.placement"),
                device_count=deployment.tensor_parallel,
            )
        )
    return commands
