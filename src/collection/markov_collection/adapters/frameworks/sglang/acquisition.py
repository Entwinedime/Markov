"""SGLang/Ascend discovery recipe and native acquisition entrypoint."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from markov_collection.adapters.frameworks.sglang.session import NativeServingSession
from markov_collection.adapters.runtimes.ascend.trace import AscendTrace
from markov_collection.artifacts.json_file import write_json


class SGLangAcquisition:
    service = "sglang-profile"
    sdk_include = "/usr/local/Ascend/ascend-toolkit/latest/include"
    selector_binary = "markov_ascend_matmul_selection"
    observer_library = "libmarkov_ascend_observer.so"

    @staticmethod
    def observer_commands(directory: str) -> tuple[list[str], list[str]]:
        return (
            [
                "cmake",
                "-S",
                "/workspace/trace-sim/src/collection/adapters/runtimes/ascend",
                "-B",
                directory,
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=Release",
                f"-DMARKOV_ASCEND_INCLUDE_DIR={SGLangAcquisition.sdk_include}",
            ],
            ["cmake", "--build", directory, "-j2"],
        )

    @staticmethod
    def domain(model: dict[str, Any], configuration: dict[str, Any]) -> dict[str, Any]:
        return {
            "dtype": model["dtype"],
            "tp": configuration["tp"],
            "attention_backend": configuration["attention_backend"],
            "page_size": configuration["page_size"],
        }

    @staticmethod
    def configuration(page_size: int) -> dict[str, int]:
        return {
            "page_size": page_size,
            "max_running_requests": 2,
            "max_total_tokens": 32 * page_size,
            "max_prefill_tokens": 32 * page_size,
        }

    @staticmethod
    def row_limits(configurations: list[dict[str, Any]], max_prompt_tokens: int) -> dict[str, int]:
        members = max(configuration["max_running_requests"] for configuration in configurations)
        rows = max(max_prompt_tokens, members)
        for configuration in configurations:
            budget = configuration["max_prefill_tokens"]
            page_size = configuration["page_size"]

            # A final whole request can start while the page-aligned prefix still has positive budget.
            prefix = (budget - 1) // page_size * page_size
            rows = max(rows, budget, prefix + max_prompt_tokens)
        return {"M": rows, "B": members}

    @staticmethod
    def request(
        specification: dict[str, Any], domain: dict[str, Any], rank: int, output: str, selector: str
    ) -> dict[str, Any]:
        requests = NativeServingSession.warmup_requests(domain["page_size"])
        model = specification["model"]
        configurations = [
            configuration
            for configuration in specification["configurations"]
            if SGLangAcquisition.domain(model, configuration) == domain
        ]
        configurations.append(SGLangAcquisition.configuration(domain["page_size"]))
        maximum_prompt = max(
            request["prompt_tokens"] for workload in specification["workloads"] for request in workload["requests"]
        )
        return {
            "model": model["path"],
            **domain,
            "rank": rank,
            "output": output,
            "requests": requests,
            "matrix_selection": {
                "binary": selector,
                "limits": SGLangAcquisition.row_limits(configurations, maximum_prompt),
            },
        }

    @staticmethod
    def command(stage: str, request: str) -> list[str]:
        return [
            "python3",
            "-m",
            "markov_collection.adapters.frameworks.sglang.acquisition",
            stage,
            "--request",
            request,
        ]

    @staticmethod
    def capture(request: dict[str, Any]) -> None:
        NativeServingSession(
            request["model"],
            tuple(tuple(pair) for pair in request["requests"]),
            Path(request["output"]),
            True,
            request["tp"],
            request["rank"],
            configuration=SGLangAcquisition.configuration(request["page_size"]),
            matrix_selection=request["matrix_selection"],
            native_library=str(Path(request["matrix_selection"]["binary"]).resolve().with_name(SGLangAcquisition.observer_library)),
            dtype=request["dtype"],
            attention_backend=request["attention_backend"],
        ).run()
        (Path(request["output"]) / f"rank-{request['rank']}.json").stat()

    @staticmethod
    def interpret(request: dict[str, Any]) -> None:
        from markov_collection.adapters.frameworks.sglang.allocation import SGLangAllocation
        from markov_collection.adapters.runtimes.ascend.indexed_writes import AscendIndexedWrites
        from markov_collection.adapters.runtimes.ascend.layouts import AscendLayouts
        from markov_collection.adapters.runtimes.pytorch.indexing import TensorAssignmentCall

        directory = Path(request["output"])
        path = directory / f"rank-{request['rank']}.json"
        source = json.loads(path.read_text())

        # Interpretation can be repeated from the captured facts without another native session.
        SGLangAllocation.declare(source["regions"], source["operations"], source["values"])
        TensorAssignmentCall.declare(source["regions"], source["values"])
        AscendIndexedWrites.declare(source["regions"], source["values"])
        AscendLayouts.declare(source["regions"], source["operations"], source["values"])
        write_json(path, source)
        if not json.loads((directory / "session-summary.json").read_text())["profile"]:
            return

        (trace,) = (directory / "native").rglob("trace_view.json")
        observer = AscendTrace(trace, directory / "runtime", device=source["environment"]["device_index"])
        physical = observer.physical_work()
        write_json(directory / "physical.json", physical)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("capture", "interpret"))
    parser.add_argument("--request", required=True, type=Path)
    arguments = parser.parse_args()
    request = json.loads(arguments.request.read_text())
    if arguments.stage == "capture":
        SGLangAcquisition.capture(request)
    else:
        SGLangAcquisition.interpret(request)
