"""Share a small bounded phase/controller capture across target configurations."""

from dataclasses import asdict
from pathlib import Path

from markov.adapters.ktransformers.capture import capture_environment, server_arguments
from markov.artifacts.capture import DeviceCommand
from markov.artifacts.request import Deployment


def sampling_commands(deployment: Deployment, report: dict, requirements: list[dict]) -> list[DeviceCommand]:
    kinds = set()
    for item in requirements:
        if "work" not in item:
            continue
        parts = item["work"]["implementation"].split(":")
        if parts[0] == "ktransformers.dense.torch":
            kinds.add((parts[1], parts[3]))
    maximum = int(report["sampling_context"]["max_input_tokens"])
    output = int(report["sampling_context"]["max_new_tokens"])
    groups = {}
    for cell in report["cells"]:
        configuration = {
            key: value for key, value in cell["configuration"].items() if key not in {"model_path", "dtype"}
        }
        kind = configuration["attention_backend"], f"threads={configuration['cpu_threads']}"
        if kind in kinds and cell["status"] != "resource_infeasible":
            groups.setdefault(kind, {})[configuration["prefill_chunk_tokens"]] = configuration

    configurations = []
    for choices in groups.values():
        chunks = sorted(chunk for chunk in choices if chunk is not None)
        selected = [None] if None in choices else []
        if chunks:
            selected.extend(sorted({chunks[0], chunks[-1]}))
        for chunk in selected:
            configuration = dict(choices[chunk])
            configuration["max_context_tokens"] = maximum + output
            configurations.append(configuration)

    commands = []
    for configuration in configurations:
        lengths = [maximum, maximum, 1, max(1, maximum // 2), max(1, maximum // 2), 1]
        counts = [output, output, 1, output, 1, output]
        workload = {
            "formal_begin_index": 0,
            "requests": [
                {
                    "prompt": None,
                    "input_ids": [0] * length,
                    "max_new_tokens": count,
                    "ignore_eos": True,
                    "return_logprob": False,
                }
                for length, count in zip(lengths, counts)
            ],
        }
        environment = capture_environment(False, True)
        payload = {
            "deployment": asdict(deployment),
            "server": server_arguments(deployment, configuration),
            "environment": environment,
            "profiling": False,
            "calibration": True,
            "workload": workload,
        }
        commands.append(
            DeviceCommand(
                "markov.adapters.ktransformers.capture",
                ("collect", "--input", "/capture/capture_input.json"),
                payload,
                "region_measurements.json",
                "calibration",
                ((Path(deployment.model), "/model", False),),
                environment,
                len(lengths),
                sum(lengths),
                sum(counts),
                "workload_report.json",
                "ktransformers_phases",
            )
        )
    return commands
